// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2016-2020 The Linux Foundation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/qpnp/qpnp-revid.h>
#include <linux/irq.h>
#include <linux/pmic-voter.h>
#include "smb-lib.h"
#include "smb-reg.h"
#include "battery.h"
#include "step-chg-jeita.h"
#include "storm-watch.h"

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#include <linux/input.h>
#if defined(CONFIG_ARCH_SONY_TAMA)
#include <linux/module.h>
#endif
#endif

#define smblib_err(chg, fmt, ...)		\
	pr_err("%s: %s: " fmt, chg->name,	\
		__func__, ##__VA_ARGS__)	\

#define smblib_dbg(chg, reason, fmt, ...)			\
	do {							\
		if (*chg->debug_mask & (reason))		\
			pr_info("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
		else						\
			pr_debug("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
	} while (0)

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
/* switch of APSD result to change SDP */
static int apsd_result_force_sdp;
module_param(apsd_result_force_sdp, int, 0644);
MODULE_PARM_DESC(apsd_result_force_sdp, "APSD result force SDP");
#endif

static bool is_secure(struct smb_charger *chg, int addr)
{
	if (addr == SHIP_MODE_REG || addr == FREQ_CLK_DIV_REG)
		return true;
	/* assume everything above 0xA0 is secure */
	return (bool)((addr & 0xFF) >= 0xA0);
}

int smblib_read(struct smb_charger *chg, u16 addr, u8 *val)
{
	unsigned int temp;
	int rc = 0;

	rc = regmap_read(chg->regmap, addr, &temp);
	if (rc >= 0)
		*val = (u8)temp;

	return rc;
}

int smblib_multibyte_read(struct smb_charger *chg, u16 addr, u8 *val,
				int count)
{
	return regmap_bulk_read(chg->regmap, addr, val, count);
}

int smblib_masked_write(struct smb_charger *chg, u16 addr, u8 mask, u8 val)
{
	int rc = 0;

	mutex_lock(&chg->write_lock);
	if (is_secure(chg, addr)) {
		rc = regmap_write(chg->regmap, (addr & 0xFF00) | 0xD0, 0xA5);
		if (rc < 0)
			goto unlock;
	}

	rc = regmap_update_bits(chg->regmap, addr, mask, val);

unlock:
	mutex_unlock(&chg->write_lock);
	return rc;
}

int smblib_write(struct smb_charger *chg, u16 addr, u8 val)
{
	int rc = 0;

	mutex_lock(&chg->write_lock);

	if (is_secure(chg, addr)) {
		rc = regmap_write(chg->regmap, (addr & ~(0xFF)) | 0xD0, 0xA5);
		if (rc < 0)
			goto unlock;
	}

	rc = regmap_write(chg->regmap, addr, val);

unlock:
	mutex_unlock(&chg->write_lock);
	return rc;
}

static int smblib_get_jeita_cc_delta(struct smb_charger *chg, int *cc_delta_ua)
{
	int rc, cc_minus_ua;
	u8 stat;

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->jeita_sw_ctl_en) {
		*cc_delta_ua = 0;
		return 0;
	}
#endif

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}

	if (!(stat & BAT_TEMP_STATUS_SOFT_LIMIT_MASK)) {
		*cc_delta_ua = 0;
		return 0;
	}

	rc = smblib_get_charge_param(chg, &chg->param.jeita_cc_comp,
					&cc_minus_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get jeita cc minus rc=%d\n", rc);
		return rc;
	}

	*cc_delta_ua = -cc_minus_ua;
	return 0;
}

int smblib_icl_override(struct smb_charger *chg, bool override)
{
	int rc;

	rc = smblib_masked_write(chg, USBIN_LOAD_CFG_REG,
				ICL_OVERRIDE_AFTER_APSD_BIT,
				override ? ICL_OVERRIDE_AFTER_APSD_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't override ICL rc=%d\n", rc);

	return rc;
}

int smblib_stat_sw_override_cfg(struct smb_charger *chg, bool override)
{
	int rc;

	/* override  = 1, SW STAT override; override = 0, HW auto mode */
	rc = smblib_masked_write(chg, STAT_CFG_REG,
				STAT_SW_OVERRIDE_CFG_BIT,
				override ? STAT_SW_OVERRIDE_CFG_BIT : 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure SW STAT override rc=%d\n",
			rc);
		return rc;
	}

	return rc;
}

/********************
 * REGISTER GETTERS *
 ********************/

int smblib_get_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int *val_u)
{
	int rc = 0;
	u8 val_raw;

	rc = smblib_read(chg, param->reg, &val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't read from 0x%04x rc=%d\n",
			param->name, param->reg, rc);
		return rc;
	}

	if (param->get_proc)
		*val_u = param->get_proc(param, val_raw);
	else
		*val_u = val_raw * param->step_u + param->min_u;
	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, *val_u, val_raw);

	return rc;
}

int smblib_get_usb_suspend(struct smb_charger *chg, int *suspend)
{
	int rc = 0;
	u8 temp;

	rc = smblib_read(chg, USBIN_CMD_IL_REG, &temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_CMD_IL rc=%d\n", rc);
		return rc;
	}
	*suspend = temp & USBIN_SUSPEND_BIT;

	return rc;
}

struct apsd_result {
	const char * const name;
	const u8 bit;
	const enum power_supply_type pst;
};

enum {
	UNKNOWN,
	SDP,
	CDP,
	DCP,
	OCP,
	FLOAT,
	HVDCP2,
	HVDCP3,
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && !defined(CONFIG_ARCH_SONY_TAMA)
	PD,
#endif
	MAX_TYPES
};

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
static const struct apsd_result const smblib_apsd_results[] = {
	[UNKNOWN] = {
		.name	= "UNKNOWN_CHARGER",
		.bit	= 0,
		.pst	= POWER_SUPPLY_TYPE_UNKNOWN
	},
	[SDP] = {
		.name	= "USB_SDP_CHARGER",
		.bit	= SDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB
	},
	[CDP] = {
		.name	= "USB_CDP_CHARGER",
		.bit	= CDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_CDP
	},
	[DCP] = {
		.name	= "USB_DCP_CHARGER",
		.bit	= DCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[OCP] = {
		.name	= "USB_PROPRIETARY_CHARGER",
		.bit	= OCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[FLOAT] = {
		.name	= "USB_FLOATED_CHARGER",
		.bit	= FLOAT_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[HVDCP2] = {
		.name	= "USB_HVDCP_CHARGER",
		.bit	= DCP_CHARGER_BIT | QC_2P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP
	},
	[HVDCP3] = {
		.name	= "USB_HVDCP_3_CHARGER",
		.bit	= DCP_CHARGER_BIT | QC_3P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP_3,
	},
#if !defined(CONFIG_ARCH_SONY_TAMA)
	[PD] = {
		.name	= "USB_PD_CHARGER",
		.bit	= 0xff,
		.pst	= POWER_SUPPLY_TYPE_USB_PD,
	},
#endif
};
#else
static const struct apsd_result const smblib_apsd_results[] = {
	[UNKNOWN] = {
		.name	= "UNKNOWN",
		.bit	= 0,
		.pst	= POWER_SUPPLY_TYPE_UNKNOWN
	},
	[SDP] = {
		.name	= "SDP",
		.bit	= SDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB
	},
	[CDP] = {
		.name	= "CDP",
		.bit	= CDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_CDP
	},
	[DCP] = {
		.name	= "DCP",
		.bit	= DCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[OCP] = {
		.name	= "OCP",
		.bit	= OCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[FLOAT] = {
		.name	= "FLOAT",
		.bit	= FLOAT_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_FLOAT
	},
	[HVDCP2] = {
		.name	= "HVDCP2",
		.bit	= DCP_CHARGER_BIT | QC_2P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP
	},
	[HVDCP3] = {
		.name	= "HVDCP3",
		.bit	= DCP_CHARGER_BIT | QC_3P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP_3,
	},
};
#endif

static const struct apsd_result *smblib_get_apsd_result(struct smb_charger *chg)
{
	int rc, i;
	u8 apsd_stat, stat;
	const struct apsd_result *result = &smblib_apsd_results[UNKNOWN];

	rc = smblib_read(chg, APSD_STATUS_REG, &apsd_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return result;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", apsd_stat);

	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT))
		return result;

	rc = smblib_read(chg, APSD_RESULT_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_RESULT_STATUS rc=%d\n",
			rc);
		return result;
	}
	stat &= APSD_RESULT_STATUS_MASK;

	for (i = 0; i < ARRAY_SIZE(smblib_apsd_results); i++) {
		if (smblib_apsd_results[i].bit == stat)
			result = &smblib_apsd_results[i];
	}

	if (apsd_stat & QC_CHARGER_BIT) {
		/* since its a qc_charger, either return HVDCP3 or HVDCP2 */
		if (result != &smblib_apsd_results[HVDCP3])
			result = &smblib_apsd_results[HVDCP2];
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER)
		result = &smblib_apsd_results[FLOAT];

	if (apsd_result_force_sdp) {
		if (result->bit != SDP_CHARGER_BIT &&
				result->bit != CDP_CHARGER_BIT) {
			smblib_err(chg, "APSD recognized as %s but read as SDP !!!!!!!\n",
					result->name);
			result = &smblib_apsd_results[SDP];
		}
	}
#endif

	return result;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
const char *smblib_somc_get_charger_type(struct smb_charger *chg)
{
	const char *charger_type = NULL;
#if defined(CONFIG_ARCH_SONY_TAMA)
	const char *charger_type_pd_name = "USB_PD_CHARGER";
	const struct apsd_result *apsd_result;

	if (chg->pd_active) {
		charger_type = charger_type_pd_name;
	} else {
		apsd_result = smblib_get_apsd_result(chg);
		charger_type = apsd_result->name;
	}
#else
	int i;

	for (i = UNKNOWN; i < MAX_TYPES; i++) {
		if (smblib_apsd_results[i].bit ==
					chg->usb_params.apsd_result_bit) {
			charger_type = smblib_apsd_results[i].name;
			break;
		}
	}
	if (!charger_type)
		charger_type = smblib_apsd_results[UNKNOWN].name;
#endif

	return charger_type;
}

static void determine_charger_type(struct smb_charger *chg)
{
	chg->charger_type_determined = true;
	power_supply_changed(chg->batt_psy);
}
#endif

/********************
 * REGISTER SETTERS *
 ********************/

static int chg_freq_list[] = {
	9600, 9600, 6400, 4800, 3800, 3200, 2700, 2400, 2100, 1900, 1700,
	1600, 1500, 1400, 1300, 1200,
};

int smblib_set_chg_freq(struct smb_chg_param *param,
				int val_u, u8 *val_raw)
{
	u8 i;

	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	/* Charger FSW is the configured freqency / 2 */
	val_u *= 2;
	for (i = 0; i < ARRAY_SIZE(chg_freq_list); i++) {
		if (chg_freq_list[i] == val_u)
			break;
	}
	if (i == ARRAY_SIZE(chg_freq_list)) {
		pr_err("Invalid frequency %d Hz\n", val_u / 2);
		return -EINVAL;
	}

	*val_raw = i;

	return 0;
}

static int smblib_set_opt_freq_buck(struct smb_charger *chg, int fsw_khz)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_buck, fsw_khz);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_buck rc=%d\n", rc);

	if (chg->mode == PARALLEL_MASTER && chg->pl.psy) {
		pval.intval = fsw_khz;
		/*
		 * Some parallel charging implementations may not have
		 * PROP_BUCK_FREQ property - they could be running
		 * with a fixed frequency
		 */
		rc = power_supply_set_property(chg->pl.psy,
				POWER_SUPPLY_PROP_BUCK_FREQ, &pval);
	}

	return rc;
}

int smblib_set_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int val_u)
{
	int rc = 0;
	u8 val_raw;

	if (param->set_proc) {
		rc = param->set_proc(param, val_u, &val_raw);
		if (rc < 0)
			return -EINVAL;
	} else {
		if (val_u > param->max_u || val_u < param->min_u) {
			smblib_err(chg, "%s: %d is out of range [%d, %d]\n",
				param->name, val_u, param->min_u, param->max_u);
			return -EINVAL;
		}

		val_raw = (val_u - param->min_u) / param->step_u;
	}

	rc = smblib_write(chg, param->reg, val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't write 0x%02x to 0x%04x rc=%d\n",
			param->name, val_raw, param->reg, rc);
		return rc;
	}

	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, val_u, val_raw);

	return rc;
}

int smblib_set_usb_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;
	int irq = chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq;

	if (suspend && irq) {
		if (chg->usb_icl_change_irq_enabled) {
			disable_irq_nosync(irq);
			chg->usb_icl_change_irq_enabled = false;
		}
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	/* WA for outbreak of icl charge irq during suspneded */
	rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				 USBIN_AICL_RERUN_EN_BIT,
				 suspend ? 0 : USBIN_AICL_RERUN_EN_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set AICL rerun en bit rc=%d\n", rc);
		return rc;
	}
#endif

	rc = smblib_masked_write(chg, USBIN_CMD_IL_REG, USBIN_SUSPEND_BIT,
				 suspend ? USBIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to USBIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	if (!suspend && irq) {
		if (!chg->usb_icl_change_irq_enabled) {
			enable_irq(irq);
			chg->usb_icl_change_irq_enabled = true;
		}
	}

	return rc;
}

int smblib_set_dc_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	/* WA for outbreak of icl charge irq during suspneded */
	rc = smblib_masked_write(chg, DCIN_AICL_OPTIONS_CFG_REG,
				 DCIN_AICL_RERUN_EN_BIT,
				 suspend ? 0 : DCIN_AICL_RERUN_EN_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set AICL rerun en bit rc=%d\n", rc);
		return rc;
	}
#endif

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_SUSPEND_BIT,
				 suspend ? DCIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to DCIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	return rc;
}

static int smblib_set_adapter_allowance(struct smb_charger *chg,
					u8 allowed_voltage)
{
	int rc = 0;

	/* PM660 only support max. 9V */
	if (chg->chg_param.smb_version == PM660_SUBTYPE) {
		switch (allowed_voltage) {
		case USBIN_ADAPTER_ALLOW_12V:
		case USBIN_ADAPTER_ALLOW_9V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_9V;
			break;
		case USBIN_ADAPTER_ALLOW_5V_OR_12V:
		case USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_5V_OR_9V;
			break;
		case USBIN_ADAPTER_ALLOW_5V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
			break;
		}
	}

	rc = smblib_write(chg, USBIN_ADAPTER_ALLOW_CFG_REG, allowed_voltage);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to USBIN_ADAPTER_ALLOW_CFG rc=%d\n",
			allowed_voltage, rc);
		return rc;
	}

	return rc;
}

#define MICRO_5V	5000000
#define MICRO_9V	9000000
#define MICRO_12V	12000000
static int smblib_set_usb_pd_allowed_voltage(struct smb_charger *chg,
					int min_allowed_uv, int max_allowed_uv)
{
	int rc;
	u8 allowed_voltage;

	if (min_allowed_uv == MICRO_5V && max_allowed_uv == MICRO_5V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V;
		smblib_set_opt_freq_buck(chg, chg->chg_freq.freq_5V);
	} else if (min_allowed_uv == MICRO_9V && max_allowed_uv == MICRO_9V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_9V;
		smblib_set_opt_freq_buck(chg, chg->chg_freq.freq_9V);
	} else if (min_allowed_uv == MICRO_12V && max_allowed_uv == MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_12V;
		smblib_set_opt_freq_buck(chg, chg->chg_freq.freq_12V);
	} else if (min_allowed_uv < MICRO_9V && max_allowed_uv <= MICRO_9V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
	} else if (min_allowed_uv < MICRO_9V && max_allowed_uv <= MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_12V;
	} else if (min_allowed_uv < MICRO_12V && max_allowed_uv <= MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_9V_TO_12V;
	} else {
		smblib_err(chg, "invalid allowed voltage [%d, %d]\n",
			min_allowed_uv, max_allowed_uv);
		return -EINVAL;
	}

	rc = smblib_set_adapter_allowance(chg, allowed_voltage);
	if (rc < 0) {
		smblib_err(chg, "Couldn't configure adapter allowance rc=%d\n",
				rc);
		return rc;
	}

	return rc;
}

/********************
 * HELPER FUNCTIONS *
 ********************/

int smblib_force_ufp(struct smb_charger *chg)
{
	int rc;

	/* force FSM in IDLE state */
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			TYPEC_DISABLE_CMD_BIT, TYPEC_DISABLE_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't put FSM in idle rc=%d\n", rc);
		return rc;
	}

	/* wait for FSM to enter idle state */
	msleep(200);

	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			VCONN_EN_VALUE_BIT | UFP_EN_CMD_BIT, UFP_EN_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't force UFP mode rc=%d\n", rc);
		return rc;
	}

	/* wait for mode change before enabling FSM */
	usleep_range(10000, 11000);

	/* release FSM from idle state */
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			TYPEC_DISABLE_CMD_BIT, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't release FSM from idle rc=%d\n", rc);
		return rc;
	}

	return 0;
}

static int smblib_request_dpdm(struct smb_charger *chg, bool enable)
{
	int rc = 0;

	if (chg->pr_swap_in_progress)
		return 0;

	/* fetch the DPDM regulator */
	if (!chg->dpdm_reg && of_get_property(chg->dev->of_node,
				"dpdm-supply", NULL)) {
		chg->dpdm_reg = devm_regulator_get(chg->dev, "dpdm");
		if (IS_ERR(chg->dpdm_reg)) {
			rc = PTR_ERR(chg->dpdm_reg);
			smblib_err(chg, "Couldn't get dpdm regulator rc=%d\n",
					rc);
			chg->dpdm_reg = NULL;
			return rc;
		}
	}

	if (enable) {
		if (chg->dpdm_reg && !regulator_is_enabled(chg->dpdm_reg)) {
			smblib_dbg(chg, PR_MISC, "enabling DPDM regulator\n");
			rc = regulator_enable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't enable dpdm regulator rc=%d\n",
					rc);
		}
	} else {
		if (chg->dpdm_reg && regulator_is_enabled(chg->dpdm_reg)) {
			smblib_dbg(chg, PR_MISC, "disabling DPDM regulator\n");
			rc = regulator_disable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't disable dpdm regulator rc=%d\n",
					rc);
		}
	}

	return rc;
}

static void smblib_rerun_apsd(struct smb_charger *chg)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "re-running APSD\n");
	if (chg->wa_flags & QC_AUTH_INTERRUPT_WA_BIT) {
		rc = smblib_masked_write(chg,
				USBIN_SOURCE_CHANGE_INTRPT_ENB_REG,
				AUTH_IRQ_EN_CFG_BIT, AUTH_IRQ_EN_CFG_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable HVDCP auth IRQ rc=%d\n",
									rc);
	}

	rc = smblib_masked_write(chg, CMD_APSD_REG,
				APSD_RERUN_BIT, APSD_RERUN_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't re-run APSD rc=%d\n", rc);
}

static const struct apsd_result *smblib_update_usb_type(struct smb_charger *chg)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

	/* if PD is active, APSD is disabled so won't have a valid result */
	if (chg->pd_active) {
		chg->real_charger_type = POWER_SUPPLY_TYPE_USB_PD;
	} else {
		/*
		 * Update real charger type only if its not FLOAT
		 * detected as as SDP
		 */
		if (!(apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT &&
			chg->real_charger_type == POWER_SUPPLY_TYPE_USB))
			chg->real_charger_type = apsd_result->pst;
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && !defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->pd_active)
		chg->usb_params.apsd_result_bit = 0xff;
	else
		chg->usb_params.apsd_result_bit = apsd_result->bit;
#endif

	smblib_dbg(chg, PR_MISC, "APSD=%s PD=%d\n",
					apsd_result->name, chg->pd_active);
	return apsd_result;
}

static int smblib_notifier_call(struct notifier_block *nb,
		unsigned long ev, void *v)
{
	struct power_supply *psy = v;
	struct smb_charger *chg = container_of(nb, struct smb_charger, nb);

	if (!strcmp(psy->desc->name, "bms")) {
		if (!chg->bms_psy)
			chg->bms_psy = psy;
		if (ev == PSY_EVENT_PROP_CHANGED)
			schedule_work(&chg->bms_update_work);
	}

	if (!chg->pl.psy && !strcmp(psy->desc->name, "parallel")) {
		chg->pl.psy = psy;
		schedule_work(&chg->pl_update_work);
	}

	return NOTIFY_OK;
}

static int smblib_register_notifier(struct smb_charger *chg)
{
	int rc;

	chg->nb.notifier_call = smblib_notifier_call;
	rc = power_supply_reg_notifier(&chg->nb);
	if (rc < 0) {
		smblib_err(chg, "Couldn't register psy notifier rc = %d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_mapping_soc_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	*val_raw = val_u << 1;

	return 0;
}

int smblib_mapping_cc_delta_to_field_value(struct smb_chg_param *param,
					   u8 val_raw)
{
	int val_u  = val_raw * param->step_u + param->min_u;

	if (val_u > param->max_u)
		val_u -= param->max_u * 2;

	return val_u;
}

int smblib_mapping_cc_delta_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u - param->max_u)
		return -EINVAL;

	val_u += param->max_u * 2 - param->min_u;
	val_u %= param->max_u * 2;
	*val_raw = val_u / param->step_u;

	return 0;
}

static void smblib_uusb_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	cancel_delayed_work_sync(&chg->pl_enable_work);

	rc = smblib_request_dpdm(chg, false);
	if (rc < 0)
		smblib_err(chg, "Couldn't to disable DPDM rc=%d\n", rc);

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	/* reset both usbin current and voltage votes */
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0);
	vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
	vote(chg->hvdcp_hw_inov_dis_votable, OV_VOTER, false, 0);

	cancel_delayed_work_sync(&chg->hvdcp_detect_work);

	if (chg->wa_flags & QC_AUTH_INTERRUPT_WA_BIT) {
		/* re-enable AUTH_IRQ_EN_CFG_BIT */
		rc = smblib_masked_write(chg,
				USBIN_SOURCE_CHANGE_INTRPT_ENB_REG,
				AUTH_IRQ_EN_CFG_BIT, AUTH_IRQ_EN_CFG_BIT);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't enable QC auth setting rc=%d\n", rc);
	}

	/* reconfigure allowed voltage for HVDCP */
	rc = smblib_set_adapter_allowance(chg,
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0)
		smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V rc=%d\n",
			rc);

	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
	chg->usb_icl_delta_ua = 0;
	chg->pulse_cnt = 0;
	chg->uusb_apsd_rerun_done = false;

	/* clear USB ICL vote for USB_PSY_VOTER */
	rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't un-vote for USB ICL rc=%d\n", rc);

	/* clear USB ICL vote for DCP_VOTER */
	rc = vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg,
			"Couldn't un-vote DCP from USB ICL rc=%d\n", rc);
}

void smblib_suspend_on_debug_battery(struct smb_charger *chg)
{
	int rc;
	union power_supply_propval val;

	if (!chg->suspend_input_on_debug_batt)
		return;

	rc = power_supply_get_property(chg->bms_psy,
			POWER_SUPPLY_PROP_DEBUG_BATTERY, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get debug battery prop rc=%d\n", rc);
		return;
	}

	vote(chg->usb_icl_votable, DEBUG_BOARD_VOTER, val.intval, 0);
	vote(chg->dc_suspend_votable, DEBUG_BOARD_VOTER, val.intval, 0);
	if (val.intval)
		pr_info("Input suspended: Fake battery\n");
}

int smblib_rerun_apsd_if_required(struct smb_charger *chg)
{
	union power_supply_propval val;
	int rc;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	const struct apsd_result *apsd_result;
#endif

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb present rc = %d\n", rc);
		return rc;
	}

	if (!val.intval)
		return 0;

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	apsd_result = smblib_get_apsd_result(chg);
	if (apsd_result->pst == POWER_SUPPLY_TYPE_USB ||
		apsd_result->pst == POWER_SUPPLY_TYPE_USB_CDP) {
		goto skip_typec_reconnection;
	}
	smblib_dbg(chg, PR_SOMC, "start type-c reconnection (%s)\n",
			apsd_result->name);

	rc = smblib_masked_write(chg,
				TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				TYPEC_DISABLE_CMD_BIT,
				TYPEC_DISABLE_CMD_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable type-c rc=%d\n", rc);

	/* wait for the adapter to turn off VBUS */
	msleep(400);

	rc = smblib_masked_write(chg,
				TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				TYPEC_DISABLE_CMD_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable type-c rc=%d\n", rc);

	/* wait for type-c detection to complete */
	msleep(200);
	smblib_dbg(chg, PR_SOMC, "complete type-c reconnection\n");
skip_typec_reconnection:
#endif

	rc = smblib_request_dpdm(chg, true);
	if (rc < 0)
		smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);

	chg->uusb_apsd_rerun_done = true;
	smblib_rerun_apsd(chg);

	return 0;
}

static int smblib_get_hw_pulse_cnt(struct smb_charger *chg, int *count)
{
	int rc;
	u8 val[2];

	switch (chg->chg_param.smb_version) {
	case PMI8998_SUBTYPE:
		rc = smblib_read(chg, QC_PULSE_COUNT_STATUS_REG, val);
		if (rc) {
			pr_err("failed to read QC_PULSE_COUNT_STATUS_REG rc=%d\n",
					rc);
			return rc;
		}
		*count = val[0] & QC_PULSE_COUNT_MASK;
		break;
	case PM660_SUBTYPE:
		rc = smblib_multibyte_read(chg,
				QC_PULSE_COUNT_STATUS_1_REG, val, 2);
		if (rc) {
			pr_err("failed to read QC_PULSE_COUNT_STATUS_1_REG rc=%d\n",
					rc);
			return rc;
		}
		*count = (val[1] << 8) | val[0];
		break;
	default:
		smblib_dbg(chg, PR_PARALLEL, "unknown SMB chip %d\n",
				chg->chg_param.smb_version);
		return -EINVAL;
	}

	return 0;
}

static int smblib_get_pulse_cnt(struct smb_charger *chg, int *count)
{
	int rc;

	/* Use software based pulse count if HW INOV is disabled */
	if (get_effective_result(chg->hvdcp_hw_inov_dis_votable) > 0) {
		*count = chg->pulse_cnt;
		return 0;
	}

	/* Use h/w pulse count if autonomous mode is enabled */
	rc = smblib_get_hw_pulse_cnt(chg, count);
	if (rc < 0)
		smblib_err(chg, "failed to read h/w pulse count rc=%d\n", rc);

	return rc;
}

#define USBIN_25MA	25000
#define USBIN_100MA	100000
#define USBIN_150MA	150000
#define USBIN_500MA	500000
#define USBIN_900MA	900000

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#define USBIN_50MA	50000
#define USBIN_75MA	75000
#define USBIN_1000MA	1000000
#define USBIN_1500MA	1500000
#define USBIN_3000MA	3000000
#else
static int set_sdp_current(struct smb_charger *chg, int icl_ua)
{
	int rc;
	u8 icl_options;
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

	/* power source is SDP */
	switch (icl_ua) {
	case USBIN_100MA:
		/* USB 2.0 100mA */
		icl_options = 0;
		break;
	case USBIN_150MA:
		/* USB 3.0 150mA */
		icl_options = CFG_USB3P0_SEL_BIT;
		break;
	case USBIN_500MA:
		/* USB 2.0 500mA */
		icl_options = USB51_MODE_BIT;
		break;
	case USBIN_900MA:
		/* USB 3.0 900mA */
		icl_options = CFG_USB3P0_SEL_BIT | USB51_MODE_BIT;
		break;
	default:
		smblib_err(chg, "ICL %duA isn't supported for SDP\n", icl_ua);
		return -EINVAL;
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB &&
		apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT) {
		/*
		 * change the float charger configuration to SDP, if this
		 * is the case of SDP being detected as FLOAT
		 */
		rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
			FORCE_FLOAT_SDP_CFG_BIT, FORCE_FLOAT_SDP_CFG_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set float ICL options rc=%d\n",
						rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		CFG_USB3P0_SEL_BIT | USB51_MODE_BIT, icl_options);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL options rc=%d\n", rc);
		return rc;
	}

	return rc;
}
#endif

static int get_sdp_current(struct smb_charger *chg, int *icl_ua)
{
	int rc;
	u8 icl_options;
	bool usb3 = false;

	rc = smblib_read(chg, USBIN_ICL_OPTIONS_REG, &icl_options);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL options rc=%d\n", rc);
		return rc;
	}

	usb3 = (icl_options & CFG_USB3P0_SEL_BIT);

	if (icl_options & USB51_MODE_BIT)
		*icl_ua = usb3 ? USBIN_900MA : USBIN_500MA;
	else
		*icl_ua = usb3 ? USBIN_150MA : USBIN_100MA;

	return rc;
}

int smblib_set_icl_current(struct smb_charger *chg, int icl_ua)
{
	int rc = 0;
	bool override;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	u8 reg;
	bool icl_setting_again = false;
#endif

	/* suspend and return if 25mA or less is requested */
	if (icl_ua <= USBIN_25MA)
		return smblib_set_usb_suspend(chg, true);

	if (icl_ua == INT_MAX)
		goto override_suspend_config;

	/* configure current */
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_dbg(chg, PR_MISC, "set icl %d uA on callback\n", icl_ua);

	rc = smblib_read(chg, USBIN_CMD_IL_REG, &reg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_CMD_IL rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	if ((reg & USBIN_SUSPEND_BIT) &&
				(icl_ua > USBIN_75MA)) {
		icl_setting_again = true;
		rc = smblib_set_charge_param(chg, &chg->param.usb_icl,
					USBIN_75MA);
	} else {
		rc = smblib_set_charge_param(chg, &chg->param.usb_icl,
					icl_ua);
	}
#else
	if (chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		&& (chg->real_charger_type == POWER_SUPPLY_TYPE_USB)) {
		rc = set_sdp_current(chg, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set SDP ICL rc=%d\n", rc);
			goto enable_icl_changed_interrupt;
		}
	} else {
		set_sdp_current(chg, 100000);
		rc = smblib_set_charge_param(chg, &chg->param.usb_icl, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
			goto enable_icl_changed_interrupt;
		}
	}
#endif

override_suspend_config:
	/* determine if override needs to be enforced */
	override = true;
#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (icl_ua == INT_MAX) {
		/* remove override if no voters - hw defaults is desired */
		override = false;
	} else if (chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) {
		if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB)
			/* For std cable with type = SDP never override */
			override = false;
		else if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_CDP
			&& icl_ua == 1500000)
			/*
			 * For std cable with type = CDP override only if
			 * current is not 1500mA
			 */
			override = false;
	}
#endif

	/* enforce override */
	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		USBIN_MODE_CHG_BIT, override ? USBIN_MODE_CHG_BIT : 0);

	rc = smblib_icl_override(chg, override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	/* unsuspend after configuring current and override */
	rc = smblib_set_usb_suspend(chg, false);
	if (rc < 0) {
		smblib_err(chg, "Couldn't resume input rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (icl_setting_again) {
		rc = smblib_set_charge_param(chg, &chg->param.usb_icl,
					icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
			goto enable_icl_changed_interrupt;
		}
	}
#endif

enable_icl_changed_interrupt:
	return rc;
}

int smblib_get_icl_current(struct smb_charger *chg, int *icl_ua)
{
	int rc = 0;
	u8 load_cfg;
	bool override;

	if (((chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT)
		|| (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB))
		&& (chg->usb_psy_desc.type == POWER_SUPPLY_TYPE_USB)) {
		rc = get_sdp_current(chg, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get SDP ICL rc=%d\n", rc);
			return rc;
		}
	} else {
		rc = smblib_read(chg, USBIN_LOAD_CFG_REG, &load_cfg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get load cfg rc=%d\n", rc);
			return rc;
		}
		override = load_cfg & ICL_OVERRIDE_AFTER_APSD_BIT;
		if (!override)
			return INT_MAX;

		/* override is set */
		rc = smblib_get_charge_param(chg, &chg->param.usb_icl, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get HC ICL rc=%d\n", rc);
			return rc;
		}
	}

	return 0;
}

int smblib_toggle_stat(struct smb_charger *chg, int reset)
{
	int rc = 0;

	if (reset) {
		rc = smblib_masked_write(chg, STAT_CFG_REG,
			STAT_SW_OVERRIDE_CFG_BIT | STAT_SW_OVERRIDE_VALUE_BIT,
			STAT_SW_OVERRIDE_CFG_BIT | 0);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't pull STAT pin low rc=%d\n", rc);
			return rc;
		}

		/*
		 * A minimum of 20us delay is expected before switching on STAT
		 * pin
		 */
		usleep_range(20, 30);

		rc = smblib_masked_write(chg, STAT_CFG_REG,
			STAT_SW_OVERRIDE_CFG_BIT | STAT_SW_OVERRIDE_VALUE_BIT,
			STAT_SW_OVERRIDE_CFG_BIT | STAT_SW_OVERRIDE_VALUE_BIT);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't pull STAT pin high rc=%d\n", rc);
			return rc;
		}

		rc = smblib_masked_write(chg, STAT_CFG_REG,
			STAT_SW_OVERRIDE_CFG_BIT | STAT_SW_OVERRIDE_VALUE_BIT,
			0);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't set hardware control rc=%d\n", rc);
			return rc;
		}
	}

	return rc;
}

static int smblib_micro_usb_disable_power_role_switch(struct smb_charger *chg,
				bool disable)
{
	int rc = 0;
	u8 power_role;

	power_role = disable ? TYPEC_DISABLE_CMD_BIT : 0;
	/* Disable pullup on CC1_ID pin and stop detection on CC pins */
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 (uint8_t)TYPEC_POWER_ROLE_CMD_MASK,
				 power_role);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			power_role, rc);
		return rc;
	}

	if (disable) {
		/* configure TypeC mode */
		rc = smblib_masked_write(chg, TYPE_C_CFG_REG,
					 TYPE_C_OR_U_USB_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure typec mode rc=%d\n",
				rc);
			return rc;
		}

		/* wait for FSM to enter idle state */
		usleep_range(5000, 5100);

		/* configure micro USB mode */
		rc = smblib_masked_write(chg, TYPE_C_CFG_REG,
					 TYPE_C_OR_U_USB_BIT,
					 TYPE_C_OR_U_USB_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure micro USB mode rc=%d\n",
				rc);
			return rc;
		}
	}

	return rc;
}

static int __smblib_set_prop_typec_power_role(struct smb_charger *chg,
				     const union power_supply_propval *val)
{
	int rc = 0;
	u8 power_role;

	switch (val->intval) {
	case POWER_SUPPLY_TYPEC_PR_NONE:
		power_role = TYPEC_DISABLE_CMD_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_DUAL:
		power_role = 0;
		break;
	case POWER_SUPPLY_TYPEC_PR_SINK:
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && !defined(CONFIG_ARCH_SONY_TAMA)
	case POWER_SUPPLY_TYPEC_PR_SINK_DELAY:
#endif
		power_role = UFP_EN_CMD_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_SOURCE:
		power_role = DFP_EN_CMD_BIT;
		break;
	default:
		smblib_err(chg, "power role %d not supported\n", val->intval);
		return -EINVAL;
	}

	if (power_role != TYPEC_DISABLE_CMD_BIT) {
		if (chg->ufp_only_mode)
			power_role = UFP_EN_CMD_BIT;
	}

	if (chg->wa_flags & TYPEC_PBS_WA_BIT) {
		if (power_role == UFP_EN_CMD_BIT) {
			/* disable PBS workaround when forcing sink mode */
			rc = smblib_write(chg, TM_IO_DTEST4_SEL, 0x0);
			if (rc < 0) {
				smblib_err(chg, "Couldn't write to TM_IO_DTEST4_SEL rc=%d\n",
					rc);
			}
		} else {
			/* restore it back to 0xA5 */
			rc = smblib_write(chg, TM_IO_DTEST4_SEL, 0xA5);
			if (rc < 0) {
				smblib_err(chg, "Couldn't write to TM_IO_DTEST4_SEL rc=%d\n",
					rc);
			}
		}
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && !defined(CONFIG_ARCH_SONY_TAMA)
	if (val->intval == POWER_SUPPLY_TYPEC_PR_SINK_DELAY) {
		smblib_dbg(chg, PR_SOMC, "power role set to SINK, delay 120ms\n");
		msleep(120);
	}
#endif

	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				TYPEC_POWER_ROLE_CMD_MASK, power_role);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			power_role, rc);
		return rc;
	}

	return rc;
}

static inline bool typec_in_src_mode(struct smb_charger *chg)
{
	return (chg->typec_mode > POWER_SUPPLY_TYPEC_NONE &&
		chg->typec_mode < POWER_SUPPLY_TYPEC_SOURCE_DEFAULT);
}

int smblib_get_prop_typec_select_rp(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc, rp;
	u8 stat;

	if (!typec_in_src_mode(chg))
		return -ENODATA;

	rc = smblib_read(chg, TYPE_C_CFG_2_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_CURRSRC_CFG_REG rc=%d\n",
				rc);
		return rc;
	}

	switch (stat & EN_80UA_180UA_CUR_SOURCE_BIT) {
	case TYPEC_SRC_RP_STD:
		rp = POWER_SUPPLY_TYPEC_SRC_RP_STD;
		break;
	case TYPEC_SRC_RP_1P5A:
		rp = POWER_SUPPLY_TYPEC_SRC_RP_1P5A;
		break;
	default:
		return -EINVAL;
	}

	val->intval = rp;

	return 0;
}

/*********************
 * VOTABLE CALLBACKS *
 *********************/

static int smblib_dc_suspend_vote_callback(struct votable *votable, void *data,
			int suspend, const char *client)
{
	struct smb_charger *chg = data;

	/* resume input if suspend is invalid */
	if (suspend < 0)
		suspend = 0;

	return smblib_set_dc_suspend(chg, (bool)suspend);
}

static int smblib_dc_icl_vote_callback(struct votable *votable, void *data,
			int icl_ua, const char *client)
{
	struct smb_charger *chg = data;
	int rc = 0;
	bool suspend;

	if (icl_ua < 0) {
		smblib_dbg(chg, PR_MISC, "No Voter hence suspending\n");
		icl_ua = 0;
	}

	suspend = (icl_ua <= USBIN_25MA);
	if (suspend)
		goto suspend;

	rc = smblib_set_charge_param(chg, &chg->param.dc_icl, icl_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set DC input current limit rc=%d\n",
			rc);
		return rc;
	}

suspend:
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	rc = vote(chg->dc_suspend_votable, DC_ICL_VOTER, suspend, 0);
#else
	rc = vote(chg->dc_suspend_votable, USER_VOTER, suspend, 0);
#endif
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s DC rc=%d\n",
			suspend ? "suspend" : "resume", rc);
		return rc;
	}
	return rc;
}

static int smblib_pd_disallowed_votable_indirect_callback(
	struct votable *votable, void *data, int disallowed, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	rc = vote(chg->pd_allowed_votable, PD_DISALLOWED_INDIRECT_VOTER,
		!disallowed, 0);

	return rc;
}

static int smblib_awake_vote_callback(struct votable *votable, void *data,
			int awake, const char *client)
{
	struct smb_charger *chg = data;

	if (awake)
		pm_stay_awake(chg->dev);
	else
		pm_relax(chg->dev);

	return 0;
}

static int smblib_chg_disable_vote_callback(struct votable *votable, void *data,
			int chg_disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 chg_disable ? 0 : CHARGING_ENABLE_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s charging rc=%d\n",
			chg_disable ? "disable" : "enable", rc);
		return rc;
	}

	return 0;
}

static int smblib_hvdcp_enable_vote_callback(struct votable *votable,
			void *data,
			int hvdcp_enable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;
	u8 val = HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT;
	u8 stat;

	/* vote to enable/disable HW autonomous INOV */
	vote(chg->hvdcp_hw_inov_dis_votable, client, !hvdcp_enable, 0);

	/*
	 * Disable the autonomous bit and auth bit for disabling hvdcp.
	 * This ensures only qc 2.0 detection runs but no vbus
	 * negotiation happens.
	 */
	if (!hvdcp_enable)
		val = HVDCP_EN_BIT;

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (chg->somc_hvdcp_disable_by_dt)
		val = 0;
#endif

	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
				 HVDCP_EN_BIT | HVDCP_AUTH_ALG_EN_CFG_BIT,
				 val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s hvdcp rc=%d\n",
			hvdcp_enable ? "enable" : "disable", rc);
		return rc;
	}

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD status rc=%d\n", rc);
		return rc;
	}

	/* re-run APSD if HVDCP was detected */
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (!chg->somc_hvdcp_disable_by_dt && hvdcp_enable) {
		vote(chg->usb_icl_votable, HIGH_VOLTAGE_VOTER, true,
						chg->high_voltage_icl_ua);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, false, 0);
		smblib_rerun_apsd(chg);
	}
#else
	if (stat & QC_CHARGER_BIT)
		smblib_rerun_apsd(chg);
#endif

	return 0;
}

static int smblib_hvdcp_disable_indirect_vote_callback(struct votable *votable,
			void *data, int hvdcp_disable, const char *client)
{
	struct smb_charger *chg = data;

	vote(chg->hvdcp_enable_votable, HVDCP_INDIRECT_VOTER,
			!hvdcp_disable, 0);

	return 0;
}

static int smblib_apsd_disable_vote_callback(struct votable *votable,
			void *data,
			int apsd_disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	if (apsd_disable) {
		rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
							AUTO_SRC_DETECT_BIT,
							0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable APSD rc=%d\n", rc);
			return rc;
		}
	} else {
		rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
							AUTO_SRC_DETECT_BIT,
							AUTO_SRC_DETECT_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable APSD rc=%d\n", rc);
			return rc;
		}
	}

	return 0;
}

static int smblib_hvdcp_hw_inov_dis_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	if (disable) {
		/*
		 * the pulse count register get zeroed when autonomous mode is
		 * disabled. Track that in variables before disabling
		 */
		rc = smblib_get_hw_pulse_cnt(chg, &chg->pulse_cnt);
		if (rc < 0) {
			pr_err("failed to read QC_PULSE_COUNT_STATUS_REG rc=%d\n",
					rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
			HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT,
			disable ? 0 : HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s hvdcp rc=%d\n",
				disable ? "disable" : "enable", rc);
		return rc;
	}

	return rc;
}

static int smblib_usb_irq_enable_vote_callback(struct votable *votable,
				void *data, int enable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[INPUT_CURRENT_LIMIT_IRQ].irq ||
				!chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		return 0;

	if (enable) {
		enable_irq(chg->irq_info[INPUT_CURRENT_LIMIT_IRQ].irq);
		enable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	} else {
		disable_irq(chg->irq_info[INPUT_CURRENT_LIMIT_IRQ].irq);
		disable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	}

	return 0;
}

static int smblib_typec_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[TYPE_C_CHANGE_IRQ].irq)
		return 0;

	if (disable)
		disable_irq_nosync(chg->irq_info[TYPE_C_CHANGE_IRQ].irq);
	else
		enable_irq(chg->irq_info[TYPE_C_CHANGE_IRQ].irq);

	return 0;
}

static int smblib_disable_power_role_switch_callback(struct votable *votable,
			void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;
	union power_supply_propval pval;
	int rc = 0;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		rc = smblib_micro_usb_disable_power_role_switch(chg, disable);
	} else {
		pval.intval = disable ? POWER_SUPPLY_TYPEC_PR_SINK
				      : POWER_SUPPLY_TYPEC_PR_DUAL;
		rc = __smblib_set_prop_typec_power_role(chg, &pval);
	}

	if (rc)
		smblib_err(chg, "power_role_switch = %s failed, rc=%d\n",
				disable ? "disabled" : "enabled", rc);
	else
		smblib_dbg(chg, PR_MISC, "power_role_switch = %s\n",
				disable ? "disabled" : "enabled");

	return rc;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
/*****************
 * USB CALLBACKS *
 *****************/

int somc_usb_register(struct smb_charger *chg)
{
	struct usb_somc_params *params = &chg->usb_params;
	struct somc_usb_ocp *ocp = &params->ocp;

	memset(&ocp->notification, 0, sizeof(ocp->notification));
	spin_lock_init(&ocp->lock);

	smblib_dbg(chg, PR_SOMC, "somc usb register success\n");
	return 0;
}

void somc_usb_unregister(struct smb_charger *chg) {}
#endif

/*******************
 * VCONN REGULATOR *
 * *****************/

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
const char *rdev_get_name(struct regulator_dev *rdev)
{
	if (rdev->constraints && rdev->constraints->name)
		return rdev->constraints->name;
	else if (rdev->desc->name)
		return rdev->desc->name;
	else
		return "";
}

int somc_usb_otg_regulator_register_ocp_notification(
			struct regulator_dev *rdev,
			struct regulator_ocp_notification *notification)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	struct somc_usb_ocp *ocp = &chg->usb_params.ocp;
	unsigned long flags;

	spin_lock_irqsave(&ocp->lock, flags);
	if (notification)
		/* register ocp notification */
		ocp->notification = *notification;
	else
		/* unregister ocp notification */
		memset(&ocp->notification, 0, sizeof(ocp->notification));
	spin_unlock_irqrestore(&ocp->lock, flags);

	pr_info("%s: registered ocp notification(notify=%p, ctxt=%p)\n",
						rdev_get_name(rdev),
						ocp->notification.notify,
						ocp->notification.ctxt);

	return 0;
}

static int somc_usb_otg_regulator_ocp_notify(struct smb_charger *chg)
{
	struct somc_usb_ocp *ocp = &chg->usb_params.ocp;
	unsigned long flags;

	spin_lock_irqsave(&ocp->lock, flags);
	if (ocp->notification.notify)
		ocp->notification.notify(ocp->notification.ctxt);
	spin_unlock_irqrestore(&ocp->lock, flags);

	return 0;
}
#endif

#define MAX_OTG_SS_TRIES 2
static int _smblib_vconn_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
	u8 val;

	/*
	 * When enabling VCONN using the command register the CC pin must be
	 * selected. VCONN should be supplied to the inactive CC pin hence using
	 * the opposite of the CC_ORIENTATION_BIT.
	 */
	smblib_dbg(chg, PR_OTG, "enabling VCONN\n");
	val = chg->typec_status[3] &
			CC_ORIENTATION_BIT ? 0 : VCONN_EN_ORIENTATION_BIT;
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 VCONN_EN_VALUE_BIT | VCONN_EN_ORIENTATION_BIT,
				 VCONN_EN_VALUE_BIT | val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable vconn setting rc=%d\n", rc);
		return rc;
	}

	return rc;
}

int smblib_vconn_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	mutex_lock(&chg->vconn_oc_lock);
	if (chg->vconn_en)
		goto unlock;

	rc = _smblib_vconn_regulator_enable(rdev);
	if (rc >= 0)
		chg->vconn_en = true;

unlock:
	mutex_unlock(&chg->vconn_oc_lock);
	return rc;
}

static int _smblib_vconn_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	smblib_dbg(chg, PR_OTG, "disabling VCONN\n");
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 VCONN_EN_VALUE_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable vconn regulator rc=%d\n", rc);

	return rc;
}

int smblib_vconn_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	mutex_lock(&chg->vconn_oc_lock);
	if (!chg->vconn_en)
		goto unlock;

	rc = _smblib_vconn_regulator_disable(rdev);
	if (rc >= 0)
		chg->vconn_en = false;

unlock:
	mutex_unlock(&chg->vconn_oc_lock);
	return rc;
}

int smblib_vconn_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&chg->vconn_oc_lock);
	ret = chg->vconn_en;
	mutex_unlock(&chg->vconn_oc_lock);
	return ret;
}

/*****************
 * OTG REGULATOR *
 *****************/
#define MAX_RETRY		15
#define MIN_DELAY_US		2000
#define MAX_DELAY_US		9000
static int otg_current[] = {250000, 500000, 1000000, 1500000};
static int smblib_enable_otg_wa(struct smb_charger *chg)
{
	u8 stat;
	int rc, i, retry_count = 0, min_delay = MIN_DELAY_US;

	for (i = 0; i < ARRAY_SIZE(otg_current); i++) {
		smblib_dbg(chg, PR_OTG, "enabling OTG with %duA\n",
						otg_current[i]);
		rc = smblib_set_charge_param(chg, &chg->param.otg_cl,
						otg_current[i]);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set otg limit rc=%d\n", rc);
			return rc;
		}

		rc = smblib_write(chg, CMD_OTG_REG, OTG_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
			return rc;
		}

		retry_count = 0;
		min_delay = MIN_DELAY_US;
		do {
			usleep_range(min_delay, min_delay + 100);
			rc = smblib_read(chg, OTG_STATUS_REG, &stat);
			if (rc < 0) {
				smblib_err(chg, "Couldn't read OTG status rc=%d\n",
							rc);
				goto out;
			}

			if (stat & BOOST_SOFTSTART_DONE_BIT) {
				rc = smblib_set_charge_param(chg,
					&chg->param.otg_cl, chg->otg_cl_ua);
				if (rc < 0) {
					smblib_err(chg, "Couldn't set otg limit rc=%d\n",
							rc);
					goto out;
				}
				break;
			}
			/* increase the delay for following iterations */
			if (retry_count > 5)
				min_delay = MAX_DELAY_US;

		} while (retry_count++ < MAX_RETRY);

		if (retry_count >= MAX_RETRY) {
			smblib_dbg(chg, PR_OTG, "OTG enable failed with %duA\n",
								otg_current[i]);
			rc = smblib_write(chg, CMD_OTG_REG, 0);
			if (rc < 0) {
				smblib_err(chg, "disable OTG rc=%d\n", rc);
				goto out;
			}
		} else {
			smblib_dbg(chg, PR_OTG, "OTG enabled\n");
			return 0;
		}
	}

	if (i == ARRAY_SIZE(otg_current)) {
		rc = -EINVAL;
		goto out;
	}

	return 0;
out:
	smblib_write(chg, CMD_OTG_REG, 0);
	return rc;
}

static int _smblib_vbus_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	smblib_dbg(chg, PR_OTG, "halt 1 in 8 mode\n");
	rc = smblib_masked_write(chg, OTG_ENG_OTG_CFG_REG,
				 ENG_BUCKBOOST_HALT1_8_MODE_BIT,
				 ENG_BUCKBOOST_HALT1_8_MODE_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set OTG_ENG_OTG_CFG_REG rc=%d\n",
			rc);
		return rc;
	}

	smblib_dbg(chg, PR_OTG, "enabling OTG\n");

	if ((chg->wa_flags & OTG_WA) && (!chg->reddragon_ipc_wa)) {
		rc = smblib_enable_otg_wa(chg);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
	} else {
		rc = smblib_write(chg, CMD_OTG_REG, OTG_EN_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
	}

	return rc;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
#define SLEEP_TIME_AFTER_DC_SUSPEND_MS			500
#define SLEEP_TIME_AFTER_DISABLING_WIRELESS_MS		25
#define SLEEP_LIMIT_COUNT_AFTER_DISABLING_WIRELESS	80
#endif
int smblib_vbus_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	int cnt = 0;
	u8 stat = 0;
	bool dc_present;
#endif

	mutex_lock(&chg->otg_oc_lock);
	if (chg->otg_en)
		goto unlock;

	if (!chg->usb_icl_votable) {
		chg->usb_icl_votable = find_votable("USB_ICL");

		if (!chg->usb_icl_votable) {
			rc = -EINVAL;
			goto unlock;
		}
	}
	vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, true, 0);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->wireless_enable) {
		vote(chg->dc_icl_votable, DC_OV_BY_OTG_VOTER, true, 0);

		rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &stat);
		if (rc < 0)
			smblib_err(chg, "Couldn't read DCIN_INT_RT_STS rc=%d\n",
									rc);

		dc_present = (bool)(stat & DCIN_PLUGIN_RT_STS_BIT);
		if (dc_present) {
			smblib_dbg(chg, PR_SOMC, "wait for preventing DC OV\n");
			msleep(SLEEP_TIME_AFTER_DC_SUSPEND_MS);
		}

		chg->vbus_reg_en = true;
		smblib_dbg(chg, PR_SOMC,
				"Disable WLC due to VBUS regulator enable\n");
		smblib_somc_handle_wireless_exclusion(chg);

		while (cnt < SLEEP_LIMIT_COUNT_AFTER_DISABLING_WIRELESS) {
			cnt++;
			rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET,
									&stat);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't read DCIN_INT_RT_STS rc=%d\n",
					rc);

			dc_present = (bool)(stat & DCIN_PLUGIN_RT_STS_BIT);
			if (dc_present) {
				smblib_dbg(chg, PR_MISC,
					   "wait for VBUS stability %d\n", cnt);
				msleep(SLEEP_TIME_AFTER_DISABLING_WIRELESS_MS);
			} else {
				smblib_dbg(chg, PR_SOMC,
					   "wait done for VBUS stability\n");
				break;
			}
		}
	}
#endif

	rc = _smblib_vbus_regulator_enable(rdev);
	if (rc >= 0)
		chg->otg_en = true;
	else
		vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->wireless_enable && !chg->otg_en) {
		vote(chg->dc_icl_votable, DC_OV_BY_OTG_VOTER, false, 0);
		chg->vbus_reg_en = false;
		smblib_dbg(chg, PR_SOMC,
			  "Enable WLC due to error of VBUS regulator enable\n");
		smblib_somc_handle_wireless_exclusion(chg);
	}
#endif

unlock:
	mutex_unlock(&chg->otg_oc_lock);
	return rc;
}

static int _smblib_vbus_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	if (chg->wa_flags & OTG_WA) {
		/* set OTG current limit to minimum value */
		rc = smblib_set_charge_param(chg, &chg->param.otg_cl,
						chg->param.otg_cl.min_u);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't set otg current limit rc=%d\n", rc);
			return rc;
		}
	}

	smblib_dbg(chg, PR_OTG, "disabling OTG\n");
	rc = smblib_write(chg, CMD_OTG_REG, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable OTG regulator rc=%d\n", rc);
		return rc;
	}

	smblib_dbg(chg, PR_OTG, "start 1 in 8 mode\n");
	rc = smblib_masked_write(chg, OTG_ENG_OTG_CFG_REG,
				 ENG_BUCKBOOST_HALT1_8_MODE_BIT, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set OTG_ENG_OTG_CFG_REG rc=%d\n", rc);
		return rc;
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->wireless_enable) {
		vote(chg->dc_icl_votable, DC_OV_BY_OTG_VOTER, false, 0);
		chg->vbus_reg_en = false;
		smblib_dbg(chg, PR_SOMC,
				"Enable WLC due to vbus regulator disable\n");
		smblib_somc_handle_wireless_exclusion(chg);
	}
#endif

	return 0;
}

int smblib_vbus_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	mutex_lock(&chg->otg_oc_lock);
	if (!chg->otg_en)
		goto unlock;

	rc = _smblib_vbus_regulator_disable(rdev);
	if (rc >= 0)
		chg->otg_en = false;

	if (chg->usb_icl_votable)
		vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0);
unlock:
	mutex_unlock(&chg->otg_oc_lock);
	return rc;
}

int smblib_vbus_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&chg->otg_oc_lock);
	ret = chg->otg_en;
	mutex_unlock(&chg->otg_oc_lock);
	return ret;
}

/********************
 * BATT PSY GETTERS *
 ********************/

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
int smblib_get_prop_charging_enabled(struct smb_charger *chg,
				union power_supply_propval *val)
{
	smblib_get_prop_input_suspend(chg, val);
	val->intval = val->intval ? 0 : 1;
	return 0;
}

int smblib_get_prop_charge_full_design(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get prop charge_full_design rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_get_prop_charge_full(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_CHARGE_FULL, val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get prop charge_full rc=%d\n", rc);
		return rc;
	}

	return 0;
}
#endif

int smblib_get_prop_input_suspend(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	val->intval
		= (get_client_vote(chg->usb_icl_votable, USER_VOTER) == 0)
		 && get_client_vote(chg->dc_suspend_votable, USER_VOTER);
	return 0;
}

int smblib_get_prop_batt_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATIF_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATIF_INT_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = !(stat & (BAT_THERM_OR_ID_MISSING_RT_STS_BIT
					| BAT_TERMINAL_MISSING_RT_STS_BIT));

	return rc;
}

int smblib_get_prop_batt_capacity(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc = -EINVAL;

#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (chg->fake_capacity >= 0) {
		val->intval = chg->fake_capacity;
		return 0;
	}
#endif

	if (chg->bms_psy)
		rc = power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_CAPACITY, val);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (rc < 0) {
		smblib_err(chg, "Couldn't get prop capacity rc=%d\n", rc);
	} else {
		smblib_somc_handle_profile_fv_rb(chg, false);

		if (val->intval <= 0 && chg->low_batt_shutdown_enabled) {
			smblib_somc_set_low_batt_suspend_en(chg);
			chg->fake_capacity = -EINVAL;
		}

		if (chg->fake_capacity >= 0)
			val->intval = chg->fake_capacity;
	}

	smblib_somc_lrc_check(chg);
	val->intval = smblib_somc_lrc_get_capacity(chg, val->intval);
#endif

	return rc;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
static const char * const smblib_charge_type_name[] = {
	[TRICKLE_CHARGE]	  = "Trickle Charge",
	[PRE_CHARGE]		  = "Pre Charge",
	[FAST_CHARGE]		  = "Fast Charge",
	[FULLON_CHARGE]		  = "Full-on Charge",
	[TAPER_CHARGE]		  = "Taper Charge",
	[TERMINATE_CHARGE]	  = "Terminate",
	[INHIBIT_CHARGE]	  = "Inhibit",
	[DISABLE_CHARGE]	  = "Disable",
};
#define ERROR_CHARGE_TYPE	"N/A"

enum {
	TEMP_CONDITION_DEFAULT = 0,
	TEMP_CONDITION_COLD,
	TEMP_CONDITION_COOL,
	TEMP_CONDITION_NORMAL,
	TEMP_CONDITION_WARM,
	TEMP_CONDITION_HOT,
};

enum {
	FAKED_STATUS_NONE = 0,
	FAKED_STATUS_TYPEC_EN_DIS_ACTIVE,
	FAKED_STATUS_SMART,
	FAKED_STATUS_DURING_WARM_FULL,
	FAKED_STATUS_RB_WA,
#if !defined(CONFIG_ARCH_SONY_TAMA)
	FAKED_STATUS_PROFILE_RB_WA,
#endif
	FAKED_STATUS_WA_FOR_WARM_FULL,
	FAKED_STATUS_STSTEM_TEMP_LEVEL,
#if defined(CONFIG_ARCH_SONY_TAMA)
	FAKED_STATUS_WA_FOR_WIRELESS,
#endif
	FAKED_STATUS_MAX,
};

static const char * const smblib_fake_status_name[] = {
	"-",
	"Running legacy unknown WA",
	"Suspending chg due to smart charge",
	"Detected Full with warm",
	"Running JEITA Reverse Boost WA",
#if !defined(CONFIG_ARCH_SONY_TAMA)
	"Running PROFILE Reverse Boost WA",
#endif
	"Back to normal from warm(Full)",
	"Thermal Lv13",
#if defined(CONFIG_ARCH_SONY_TAMA)
	"Wireless charger short break WA",
#endif
};

#define ERROR_FAKED_STATUS_NAME "Invalid"
const char *smblib_somc_get_faked_status(struct smb_charger *chg)
{
	int stat = chg->faked_status;

	if (stat < FAKED_STATUS_NONE || stat >= FAKED_STATUS_MAX)
		return ERROR_FAKED_STATUS_NAME;
	else
		return smblib_fake_status_name[stat];
}
#endif

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#define NUM_MAX_CLIENTS		32
#endif
int smblib_get_prop_batt_status(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	bool usb_online, dc_online, qnovo_en;
	u8 stat, pt_en_cmd;
	int rc;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	int i = 0;
	int votabled, smart_votabled = 0, other_votabled = 0;
	int thermal_votabled = 0;
	char *clients[NUM_MAX_CLIENTS];
	int num_clients = 0;
#endif

	rc = smblib_get_prop_usb_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb online property rc=%d\n",
			rc);
		return rc;
	}
	usb_online = (bool)pval.intval;

	rc = smblib_get_prop_dc_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get dc online property rc=%d\n",
			rc);
		return rc;
	}
	dc_online = (bool)pval.intval;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}
	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (!usb_online && !dc_online) {
		switch (stat) {
		case TERMINATE_CHARGE:
		case INHIBIT_CHARGE:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
		chg->faked_status = FAKED_STATUS_NONE;
#endif
		return rc;
	}

	switch (stat) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FAST_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case TERMINATE_CHARGE:
	case INHIBIT_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	case DISABLE_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (chg->typec_en_dis_active || chg->duration_fake_charging) {
		smblib_dbg(chg, PR_MISC,
			"Fake charging during type-c en/dis is active (%d)\n",
								val->intval);
		val->intval = chg->status_before_typec_en_dis_active;
		chg->faked_status = FAKED_STATUS_TYPEC_EN_DIS_ACTIVE;
		return 0;
	}

	num_clients = somc_get_vote_clients(chg->chg_disable_votable, clients);
	for (i = 0; i < num_clients; i++) {
		votabled = get_client_vote(chg->chg_disable_votable,
								clients[i]);
		if (strcmp(clients[i], BATTCHG_SMART_EN_VOTER) == 0)
			smart_votabled = votabled;
		else if (strcmp(clients[i], THERMAL_DAEMON_VOTER) == 0 &&
				chg->system_temp_level ==
				chg->fake_charging_temp_level)
			thermal_votabled = votabled;
		else if (!other_votabled)
			other_votabled = votabled;
	}
	if (smart_votabled && !other_votabled) {
		smblib_dbg(chg, PR_MISC,
				"Fake charging due to smart charge (%d)\n",
								val->intval);
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		chg->faked_status = FAKED_STATUS_SMART;
		return 0;
	} else if ((thermal_votabled && !other_votabled) ||
						chg->thermal_fake_charging) {
		smblib_dbg(chg, PR_MISC,
			"Fake charging during system_temp_level is %d%s (%d)\n",
				chg->system_temp_level,
				chg->thermal_fake_charging ?
				", but in fake duration" : "",
				val->intval);
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		chg->faked_status = FAKED_STATUS_STSTEM_TEMP_LEVEL;
		return 0;
	}

	if (val->intval == POWER_SUPPLY_STATUS_FULL &&
	    chg->jeita_condition == TEMP_CONDITION_WARM) {
		smblib_dbg(chg, PR_MISC,
				"Fake charging during Warm and FULL (%d)\n",
								val->intval);
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		chg->faked_status = FAKED_STATUS_DURING_WARM_FULL;
		return 0;
	}

#if !defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->profile_fv_rb_en &&
		!get_effective_result(chg->chg_disable_votable)) {
		smblib_dbg(chg, PR_SOMC,
			"Fake charging due to PROFILE Reverse Boost WA (%d)\n",
								val->intval);
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		chg->faked_status = FAKED_STATUS_PROFILE_RB_WA;
		return 0;
	}
#endif

	if (chg->jeita_rb_warm_hi_vbatt_en &&
		!get_effective_result(chg->chg_disable_votable)) {
		smblib_dbg(chg, PR_SOMC,
			"Fake charging due to JEITA Reverse Boost WA (%d)\n",
								val->intval);
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		chg->faked_status = FAKED_STATUS_RB_WA;
		return 0;
	}

	if (chg->jeita_keep_fake_charging) {
		smblib_dbg(chg, PR_MISC,
				"Fake charging during WA for Warm/FULL (%d)\n",
								val->intval);
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		chg->faked_status = FAKED_STATUS_WA_FOR_WARM_FULL;
		return 0;
	}

#if defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->wireless_wa_fake_charging) {
		smblib_dbg(chg, PR_MISC,
				"Fake charging for wireless (%d)\n",
								val->intval);
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		chg->faked_status = FAKED_STATUS_WA_FOR_WIRELESS;
		return 0;
	}
#endif

	chg->faked_status = FAKED_STATUS_NONE;
#endif

	if (val->intval != POWER_SUPPLY_STATUS_CHARGING)
		return 0;

	if (!usb_online && dc_online
		&& chg->fake_batt_status == POWER_SUPPLY_STATUS_FULL) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
			return rc;
	}

	stat &= ENABLE_TRICKLE_BIT | ENABLE_PRE_CHARGING_BIT |
		 ENABLE_FAST_CHARGING_BIT | ENABLE_FULLON_MODE_BIT;

	rc = smblib_read(chg, QNOVO_PT_ENABLE_CMD_REG, &pt_en_cmd);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read QNOVO_PT_ENABLE_CMD_REG rc=%d\n",
				rc);
		return rc;
	}

	qnovo_en = (bool)(pt_en_cmd & QNOVO_PT_ENABLE_CMD_BIT);

	/* ignore stat7 when qnovo is enabled */
	if (!qnovo_en && !stat)
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

int smblib_get_prop_batt_charge_type(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	switch (stat & BATTERY_CHARGER_STATUS_MASK) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case FAST_CHARGE:
	case FULLON_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TAPER;
		break;
	default:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

	return rc;
}

int smblib_get_prop_batt_health(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval;
	int rc;
	int effective_fv_uv;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_2 = 0x%02x\n",
		   stat);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (stat & CHARGER_ERROR_STATUS_BAT_OV_BIT &&
	    (!chg->jeita_condition ||
	     chg->jeita_condition != TEMP_CONDITION_WARM)) {
#else
	if (stat & CHARGER_ERROR_STATUS_BAT_OV_BIT) {
#endif
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
		if (!rc) {
			/*
			 * If Vbatt is within 40mV above Vfloat, then don't
			 * treat it as overvoltage.
			 */
			effective_fv_uv = get_effective_result(chg->fv_votable);
			if (pval.intval >= effective_fv_uv + 40000) {
				val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
				smblib_err(chg, "battery over-voltage vbat_fg = %duV, fv = %duV\n",
						pval.intval, effective_fv_uv);
				goto done;
			}
		}
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#if defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->jeita_sw_ctl_en) {
		switch (chg->jeita_synth_condition) {
		case TEMP_CONDITION_HOT:
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
			break;
		case TEMP_CONDITION_WARM:
			val->intval = POWER_SUPPLY_HEALTH_WARM;
			break;
		case TEMP_CONDITION_COOL:
			val->intval = POWER_SUPPLY_HEALTH_COOL;
			break;
		case TEMP_CONDITION_COLD:
			val->intval = POWER_SUPPLY_HEALTH_COLD;
			break;
		default:
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
			break;
		}
	} else {
		if (stat & BAT_TEMP_STATUS_TOO_COLD_BIT)
			val->intval = POWER_SUPPLY_HEALTH_COLD;
		else if (stat & BAT_TEMP_STATUS_TOO_HOT_BIT)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (stat & BAT_TEMP_STATUS_COLD_SOFT_LIMIT_BIT)
			val->intval = POWER_SUPPLY_HEALTH_COOL;
		else if (stat & BAT_TEMP_STATUS_HOT_SOFT_LIMIT_BIT)
			val->intval = POWER_SUPPLY_HEALTH_WARM;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
	}
#else
	if (chg->jeita_condition) {
		if (stat & BAT_TEMP_STATUS_TOO_COLD_BIT) {
			val->intval = POWER_SUPPLY_HEALTH_COLD;
		} else if (stat & BAT_TEMP_STATUS_TOO_HOT_BIT) {
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		} else {
			switch (chg->jeita_condition) {
			case TEMP_CONDITION_HOT:
				val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
				break;
			case TEMP_CONDITION_WARM:
				val->intval = POWER_SUPPLY_HEALTH_WARM;
				break;
			case TEMP_CONDITION_COOL:
				val->intval = POWER_SUPPLY_HEALTH_COOL;
				break;
			case TEMP_CONDITION_COLD:
				val->intval = POWER_SUPPLY_HEALTH_COLD;
				break;
			default:
				val->intval = POWER_SUPPLY_HEALTH_GOOD;
				break;
			}
		}
	} else {
		if (stat & BAT_TEMP_STATUS_TOO_COLD_BIT)
			val->intval = POWER_SUPPLY_HEALTH_COLD;
		else if (stat & BAT_TEMP_STATUS_TOO_HOT_BIT)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (stat & BAT_TEMP_STATUS_COLD_SOFT_LIMIT_BIT)
			val->intval = POWER_SUPPLY_HEALTH_COOL;
		else if (stat & BAT_TEMP_STATUS_HOT_SOFT_LIMIT_BIT)
			val->intval = POWER_SUPPLY_HEALTH_WARM;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
	}
#endif
#else
	if (stat & BAT_TEMP_STATUS_TOO_COLD_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COLD;
	else if (stat & BAT_TEMP_STATUS_TOO_HOT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (stat & BAT_TEMP_STATUS_COLD_SOFT_LIMIT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COOL;
	else if (stat & BAT_TEMP_STATUS_HOT_SOFT_LIMIT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_WARM;
	else
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
#endif

done:
	return rc;
}

int smblib_get_prop_system_temp_level(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->system_temp_level;
	return 0;
}

int smblib_get_prop_system_temp_level_max(struct smb_charger *chg,
				union power_supply_propval *val)
{
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	val->intval = chg->thermal_fcc_levels;
#else
	val->intval = chg->thermal_levels;
#endif
	return 0;
}

int smblib_get_prop_input_current_limited(struct smb_charger *chg,
				union power_supply_propval *val)
{
	u8 stat;
	int rc;

	if (chg->fake_input_current_limited >= 0) {
		val->intval = chg->fake_input_current_limited;
		return 0;
	}

	rc = smblib_read(chg, AICL_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
		return rc;
	}
	val->intval = (stat & SOFT_ILIMIT_BIT) || chg->is_hdc;
	return 0;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#if defined(CONFIG_ARCH_SONY_TAMA)
#define ECELSIUS_DEGREE (-2730)
int smblib_get_prop_real_temp(struct smb_charger *chg,
			      union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	int rc;
	int batt_temp = ECELSIUS_DEGREE;
	int skin_temp = ECELSIUS_DEGREE;
	int wlc_temp = ECELSIUS_DEGREE;
	int corrected_batt_temp = ECELSIUS_DEGREE;
	int corrected_skin_temp = ECELSIUS_DEGREE;
	int corrected_wlc_temp = ECELSIUS_DEGREE;
	int real_temp = 0;

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_TEMP, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get batt temp rc=%d\n", rc);
		return rc;
	} else {
		batt_temp = pval.intval;
		corrected_batt_temp = batt_temp + chg->batt_temp_correctton;
	}

	if (chg->real_temp_use_aux) {
		rc = smblib_get_prop_skin_temp(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get skin temp rc=%d\n", rc);
		} else {
			skin_temp = pval.intval;
			corrected_skin_temp =
					skin_temp + chg->skin_temp_correctton;
		}
	}

	if (chg->real_temp_use_wlc) {
		if (!chg->wireless_psy)
			chg->wireless_psy =
					power_supply_get_by_name("wireless");

		if (chg->wireless_psy) {
			rc = power_supply_get_property(chg->wireless_psy,
							POWER_SUPPLY_PROP_TEMP,
							&pval);
			if (rc) {
				smblib_dbg(chg, PR_MISC,
					   "Couldn't get wlc_temp rc = %d\n",
					   rc);
			} else {
				wlc_temp = pval.intval;
				corrected_wlc_temp =
					wlc_temp + chg->wlc_temp_correctton;
			}
		} else {
			smblib_err(chg, "Couldn't get wireless_psy\n");
		}
	}

	real_temp = max(corrected_batt_temp,
				max(corrected_skin_temp, corrected_wlc_temp));

	smblib_dbg(chg, PR_MISC,
			"battery temp: batt=%d aux=%d wlc=%d real_temp=%d\n",
			batt_temp, skin_temp, wlc_temp, real_temp);
	val->intval = real_temp;
	return 0;
}
#else
int smblib_get_prop_real_temp(struct smb_charger *chg,
			      union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy,
				       POWER_SUPPLY_PROP_REAL_TEMP, val);
	return rc;
}
#endif
#endif

int smblib_get_prop_batt_charge_done(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	val->intval = (stat == TERMINATE_CHARGE);
	return 0;
}

int smblib_get_prop_charge_qnovo_enable(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, QNOVO_PT_ENABLE_CMD_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read QNOVO_PT_ENABLE_CMD rc=%d\n",
			rc);
		return rc;
	}

	val->intval = (bool)(stat & QNOVO_PT_ENABLE_CMD_BIT);
	return 0;
}

int smblib_get_prop_from_bms(struct smb_charger *chg,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy, psp, val);

	return rc;
}

/***********************
 * BATTERY PSY SETTERS *
 ***********************/

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
int smblib_set_prop_charging_enabled(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;
	union power_supply_propval tmp = *val;

	tmp.intval = tmp.intval ? 0 : 1;
	rc = smblib_set_prop_input_suspend(chg, &tmp);
	return rc;
}
#endif

int smblib_set_prop_input_suspend(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	int rc;

	/* vote 0mA when suspended */
	rc = vote(chg->usb_icl_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s USB rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	rc = vote(chg->dc_suspend_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s DC rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	power_supply_changed(chg->batt_psy);
	return rc;
}

int smblib_set_prop_batt_capacity(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	chg->fake_capacity = val->intval;

	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_batt_status(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	/* Faking battery full */
	if (val->intval == POWER_SUPPLY_STATUS_FULL)
		chg->fake_batt_status = val->intval;
	else
		chg->fake_batt_status = -EINVAL;

	power_supply_changed(chg->batt_psy);

	return 0;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
static void smblib_somc_thermal_fake_charging_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						thermal_fake_charging_work);
	chg->thermal_fake_charging = true;
	msleep(500);
	chg->thermal_fake_charging = false;
}
#endif

int smblib_set_prop_system_temp_level(struct smb_charger *chg,
				const union power_supply_propval *val)
{
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_dbg(chg, PR_SOMC, "Changed Thernal LV from %d to %d\n",
					chg->system_temp_level, val->intval);
	if (val->intval < 0)
		return -EINVAL;

	if ((chg->system_temp_level == chg->fake_charging_temp_level) &&
			(val->intval < chg->fake_charging_temp_level))
		schedule_work(&chg->thermal_fake_charging_work);

	chg->system_temp_level = val->intval;

	smblib_somc_thermal_fcc_change(chg);
	smblib_somc_thermal_icl_change(chg);
	power_supply_changed(chg->batt_psy);
#else
	if (val->intval < 0)
		return -EINVAL;

	if (chg->thermal_levels <= 0)
		return -EINVAL;

	if (val->intval > chg->thermal_levels)
		return -EINVAL;

	chg->system_temp_level = val->intval;

	if (chg->system_temp_level == chg->thermal_levels)
		return vote(chg->chg_disable_votable,
			THERMAL_DAEMON_VOTER, true, 0);

	vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER, false, 0);
	if (chg->system_temp_level == 0)
		return vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, false, 0);

	vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, true,
			chg->thermal_mitigation[chg->system_temp_level]);
#endif
	return 0;
}

int smblib_set_prop_charge_qnovo_enable(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_masked_write(chg, QNOVO_PT_ENABLE_CMD_REG,
			QNOVO_PT_ENABLE_CMD_BIT,
			val->intval ? QNOVO_PT_ENABLE_CMD_BIT : 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't enable qnovo rc=%d\n", rc);
		return rc;
	}

	return rc;
}

int smblib_set_prop_input_current_limited(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	chg->fake_input_current_limited = val->intval;
	return 0;
}

int smblib_rerun_aicl(struct smb_charger *chg)
{
	int rc, settled_icl_ua;
	u8 stat;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
								rc);
		return rc;
	}

	/* USB is suspended so skip re-running AICL */
	if (stat & USBIN_SUSPEND_STS_BIT)
		return rc;

	smblib_dbg(chg, PR_MISC, "re-running AICL\n");
	rc = smblib_get_charge_param(chg, &chg->param.icl_stat,
			&settled_icl_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get settled ICL rc=%d\n", rc);
		return rc;
	}

	vote(chg->usb_icl_votable, AICL_RERUN_VOTER, true,
			max(settled_icl_ua - chg->param.usb_icl.step_u,
				chg->param.usb_icl.step_u));
	vote(chg->usb_icl_votable, AICL_RERUN_VOTER, false, 0);

	return 0;
}

static int smblib_dp_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 increment */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_INCREMENT_BIT,
			SINGLE_INCREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static int smblib_dm_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 decrement */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_DECREMENT_BIT,
			SINGLE_DECREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static int smblib_force_vbus_voltage(struct smb_charger *chg, u8 val)
{
	int rc;

	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, val, val);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

int smblib_dp_dm(struct smb_charger *chg, int val)
{
	int target_icl_ua, rc = 0;
	union power_supply_propval pval;

	switch (val) {
	case POWER_SUPPLY_DP_DM_DP_PULSE:
		rc = smblib_dp_pulse(chg);
		if (!rc)
			chg->pulse_cnt++;
		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DP_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_DM_PULSE:
		rc = smblib_dm_pulse(chg);
		if (!rc && chg->pulse_cnt)
			chg->pulse_cnt--;
		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DM_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_ICL_DOWN:
		target_icl_ua = get_effective_result(chg->usb_icl_votable);
		if (target_icl_ua < 0) {
			/* no client vote, get the ICL from charger */
			rc = power_supply_get_property(chg->usb_psy,
					POWER_SUPPLY_PROP_HW_CURRENT_MAX,
					&pval);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't get max current rc=%d\n",
					rc);
				return rc;
			}
			target_icl_ua = pval.intval;
		}

		/*
		 * Check if any other voter voted on USB_ICL in case of
		 * voter other than SW_QC3_VOTER reset and restart reduction
		 * again.
		 */
		if (target_icl_ua != get_client_vote(chg->usb_icl_votable,
							SW_QC3_VOTER))
			chg->usb_icl_delta_ua = 0;

		chg->usb_icl_delta_ua += 100000;
		vote(chg->usb_icl_votable, SW_QC3_VOTER, true,
						target_icl_ua - 100000);
		smblib_dbg(chg, PR_PARALLEL, "ICL DOWN ICL=%d reduction=%d\n",
				target_icl_ua, chg->usb_icl_delta_ua);
		break;
	case POWER_SUPPLY_DP_DM_FORCE_5V:
		rc = smblib_force_vbus_voltage(chg, FORCE_5V_BIT);
		if (rc < 0)
			pr_err("Failed to force 5V\n");
		break;
	case POWER_SUPPLY_DP_DM_FORCE_9V:
		/* Force 1A ICL before requesting higher voltage */
		vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, true, 1000000);
		rc = smblib_force_vbus_voltage(chg, FORCE_9V_BIT);
		if (rc < 0)
			pr_err("Failed to force 9V\n");
		break;
	case POWER_SUPPLY_DP_DM_FORCE_12V:
		/* Force 1A ICL before requesting higher voltage */
		vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, true, 1000000);
		rc = smblib_force_vbus_voltage(chg, FORCE_12V_BIT);
		if (rc < 0)
			pr_err("Failed to force 12V\n");
		break;
	case POWER_SUPPLY_DP_DM_ICL_UP:
	default:
		break;
	}

	return rc;
}

int smblib_disable_hw_jeita(struct smb_charger *chg, bool disable)
{
#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
	int rc;
	u8 mask;

	/*
	 * Disable h/w base JEITA compensation if s/w JEITA is enabled
	 */
	mask = JEITA_EN_COLD_SL_FCV_BIT
		| JEITA_EN_HOT_SL_FCV_BIT
		| JEITA_EN_HOT_SL_CCC_BIT
		| JEITA_EN_COLD_SL_CCC_BIT,
	rc = smblib_masked_write(chg, JEITA_EN_CFG_REG, mask,
			disable ? 0 : mask);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure s/w jeita rc=%d\n",
			rc);
		return rc;
	}
#endif
	return 0;
}

/*******************
 * DC PSY GETTERS *
 *******************/

int smblib_get_prop_dc_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DCIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & DCIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_dc_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	int org_online;

	if (get_client_vote(chg->dc_suspend_votable, LOW_BATT_EN_VOTER)) {
		val->intval = false;
		return rc;
	}
#endif

	if (get_client_vote(chg->dc_suspend_votable, USER_VOTER)) {
		val->intval = false;
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_DCIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (!val->intval && chg->wireless_wa_fake_charging) {
		smblib_dbg(chg, PR_SOMC,
				"Fake online for wireless (%d)\n",
								val->intval);
		val->intval = 1;
		return 0;
	}
	org_online = val->intval;
	rc = smblib_get_prop_dc_present(chg, val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get present rc=%d\n", rc);
		return rc;
	}
	if (!!val->intval != !!org_online)
		smblib_dbg(chg, PR_MISC,
			"online mismatch: ret=%d, POWER_PATH_STATUS=0x%02x\n",
							val->intval, stat);
#endif

	return rc;
}

int smblib_get_prop_dc_current_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	val->intval = get_effective_result_locked(chg->dc_icl_votable);
	return 0;
}

/*******************
 * DC PSY SETTERS *
 * *****************/

int smblib_set_prop_dc_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	rc = vote(chg->dc_icl_votable, USER_VOTER, true, val->intval);
	return rc;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
#define WIRELESS_MODE_OFF	0
#define WIRELESS_MODE_5V	1
#define WIRELESS_MODE_9V	2
#define WIRELESS_5V_ICL		700000
#define WIRELESS_9V_ICL		1000000
#define WIRELESS_5V_AICL_TRESH	0x00
#define WIRELESS_9V_AICL_TRESH	0x00 /* 4.0V (instead of 0x1c/8.0V) */
#define WIRELESS_AICL_TRESH_MIN	0x00
#define WIRELESS_AICL_TRESH_MAX	0x2F
int smblib_set_dcin_aicl_thresh(struct smb_charger *chg)
{
	int rc;
	u8 val;

	if (chg->debug_dcin_aicl_thresh_enable &&
		chg->debug_dcin_aicl_thresh_raw >= WIRELESS_AICL_TRESH_MIN &&
		chg->debug_dcin_aicl_thresh_raw <= WIRELESS_AICL_TRESH_MAX)
		val = chg->debug_dcin_aicl_thresh_raw;
	else
		val = chg->dcin_aicl_thresh_raw;

	smblib_dbg(chg, PR_SOMC, "DCIN_CONT_AICL_THRESHOLD_CFG: 0x%02x\n", val);
	rc = smblib_masked_write(chg, DCIN_AICL_REF_SEL_CFG_REG,
					DCIN_CONT_AICL_THRESHOLD_CFG_MASK, val);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't set rc=%d\n", rc);

	return rc;
}

#define WIRELESS_WA_1ST_FAKE_TIME_MS 650
#define WIRELESS_WA_2ND_FAKE_TIME_MS 2500
static void smblib_somc_wlc_fake_charging_work(struct work_struct *work)
{
	int rc;
	union power_supply_propval pval = {0, };
	struct smb_charger *chg = container_of(work, struct smb_charger,
						wireless_wa_fake_charging_work);

	rc = smblib_get_prop_batt_status(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read batt_status rc=%d\n", rc);
		return;
	}
	if (pval.intval != POWER_SUPPLY_STATUS_CHARGING &&
			pval.intval != POWER_SUPPLY_STATUS_FULL) {
		smblib_dbg(chg, PR_SOMC, "don't enforce wireless fake\n");
		return;
	}

	chg->wireless_wa_fake_charging = true;
	smblib_dbg(chg, PR_SOMC, "wireless fake phase-1 is started.\n");
	msleep(WIRELESS_WA_1ST_FAKE_TIME_MS);
	smblib_dbg(chg, PR_SOMC, "wireless fake phase-1 is terminated.\n");

	if (!chg->wireless_psy)
		chg->wireless_psy = power_supply_get_by_name("wireless");

	if (chg->wireless_psy) {
		rc = power_supply_get_property(chg->wireless_psy,
					POWER_SUPPLY_PROP_WIRELESS_STATUS,
					&pval);
		if (rc) {
			smblib_dbg(chg, PR_SOMC,
				   "Couldn't get wireless status rc = %d\n",
				   rc);
			goto out;
		}
		if (strcmp(pval.strval, "negotiating") == 0) {
			smblib_dbg(chg, PR_SOMC,
				"wireless fake phase-2 is started.\n");
			msleep(WIRELESS_WA_2ND_FAKE_TIME_MS);
			smblib_dbg(chg, PR_SOMC,
				"wireless fake phase-2 is terminated.\n");
		}
	}
out:
	chg->wireless_wa_fake_charging = false;
	power_supply_changed(chg->batt_psy);
}

int smblib_set_prop_wireless_mode(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;
	int mode;
	int current_ua = 0;
	bool icl_boost = false;
	union power_supply_propval pval = {0, };

	if (!chg->wireless_enable)
		return 0;

	mode = val->intval;

	/* set AICL Threshold */
	switch (val->intval) {
	case WIRELESS_MODE_9V:
		chg->dcin_aicl_thresh_raw = WIRELESS_9V_AICL_TRESH;
		break;
	case WIRELESS_MODE_OFF:
	case WIRELESS_MODE_5V:
	default:
		chg->dcin_aicl_thresh_raw = WIRELESS_5V_AICL_TRESH;
		break;
	}
	rc = smblib_set_dcin_aicl_thresh(chg);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't set rc=%d\n", rc);

	if (!chg->wireless_psy)
		chg->wireless_psy = power_supply_get_by_name("wireless");

	if (chg->wireless_psy) {
		rc = power_supply_get_property(chg->wireless_psy,
						POWER_SUPPLY_PROP_AUTH, &pval);
		if (rc < 0)
			smblib_dbg(chg, PR_SOMC,
				   "Couldn't get wireless auth rc = %d\n",
				   rc);
		else
			icl_boost = (bool)pval.intval;
	}

	/* set ICL */
	switch (mode) {
	case WIRELESS_MODE_5V:
		if (chg->dc_l_volt_icl_ua)
			current_ua = chg->dc_l_volt_icl_ua;
		break;
	case WIRELESS_MODE_9V:
		if (icl_boost && chg->dc_h_volt_boost_icl_ua)
			current_ua = chg->dc_h_volt_boost_icl_ua;
		else if (chg->dc_h_volt_icl_ua)
			current_ua = chg->dc_h_volt_icl_ua;
		break;
	case WIRELESS_MODE_OFF:
	default:
		current_ua = 0;
		break;
	}
	smblib_dbg(chg, PR_SOMC, "DCIN icl: %d, mode:%d, icl_boost:%d\n",
					current_ua, mode, (int)icl_boost);
	rc = vote(chg->dc_icl_votable, WIRELESS_VOTER, true, current_ua);
	return rc;
}
#endif

/*******************
 * USB PSY GETTERS *
 *******************/

int smblib_get_prop_usb_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	return 0;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#define SDP_CURRENT_SUSPENDED 2000
#endif
int smblib_get_prop_usb_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	int org_online;
	int sdp_current;

	if (get_client_vote(chg->usb_icl_votable, LOW_BATT_EN_VOTER) == 0) {
		val->intval = false;
		return rc;
	}
	if (get_client_vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER) == 0) {
		val->intval = false;
		return rc;
	}
	sdp_current = get_client_vote(chg->usb_icl_votable, USB_PSY_VOTER);
	if (sdp_current >= 0 && sdp_current <= SDP_CURRENT_SUSPENDED) {
		val->intval = false;
		return rc;
	}
#endif

	if (get_client_vote_locked(chg->usb_icl_votable, USER_VOTER) == 0) {
		val->intval = false;
		return rc;
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (chg->typec_en_dis_active) {
		val->intval = true;
		return rc;
	}
#endif

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_USBIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	org_online = val->intval;
	rc = smblib_get_prop_usb_present(chg, val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get present rc=%d\n", rc);
		return rc;
	}
	if (!!val->intval != !!org_online)
		smblib_dbg(chg, PR_MISC,
		"online mismatch: ret=%d, POWER_PATH_STATUS=0x%02x\n",
						val->intval, stat);
#endif
	return rc;
}

int smblib_get_prop_usb_voltage_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		if (chg->chg_param.smb_version == PM660_SUBTYPE)
			val->intval = MICRO_9V;
		else
			val->intval = MICRO_12V;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_max_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_usb_voltage_max_design(struct smb_charger *chg,
					union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
	case POWER_SUPPLY_TYPE_USB_PD:
		if (chg->chg_param.smb_version == PM660_SUBTYPE)
			val->intval = MICRO_9V;
		else
#if defined(CONFIG_ARCH_SONY_TAMA)
			val->intval = MICRO_9V;
#else
			val->intval = MICRO_12V;
#endif
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_usb_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	if (!chg->iio.usbin_v_chan ||
		PTR_ERR(chg->iio.usbin_v_chan) == -EPROBE_DEFER)
		chg->iio.usbin_v_chan = iio_channel_get(chg->dev, "usbin_v");

	if (IS_ERR(chg->iio.usbin_v_chan))
		return PTR_ERR(chg->iio.usbin_v_chan);

	return iio_read_channel_processed(chg->iio.usbin_v_chan, &val->intval);
}

int smblib_get_prop_usb_current_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_get_prop_usb_present(chg, val);
	if (rc < 0 || !val->intval)
		return rc;

	if (!chg->iio.usbin_i_chan ||
		PTR_ERR(chg->iio.usbin_i_chan) == -EPROBE_DEFER)
		chg->iio.usbin_i_chan = iio_channel_get(chg->dev, "usbin_i");

	if (IS_ERR(chg->iio.usbin_i_chan))
		return PTR_ERR(chg->iio.usbin_i_chan);

	return iio_read_channel_processed(chg->iio.usbin_i_chan, &val->intval);
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
int smblib_get_prop_skin_temp(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc = 0;

	if (!chg->iio.skin_temp_chan ||
		PTR_ERR(chg->iio.skin_temp_chan) == -EPROBE_DEFER)
		chg->iio.skin_temp_chan = iio_channel_get(chg->dev,
								"skin_temp");

	if (IS_ERR(chg->iio.skin_temp_chan))
		return PTR_ERR(chg->iio.skin_temp_chan);

	rc = iio_read_channel_processed(chg->iio.skin_temp_chan, &val->intval);
	val->intval /= 100;
	return rc;
}

#if defined(CONFIG_ARCH_SONY_TAMA)
int smblib_get_prop_dc_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_get_prop_dc_present(chg, val);
	if (rc < 0 || !val->intval)
		return rc;

	if (!chg->iio.dcin_v_chan ||
		PTR_ERR(chg->iio.dcin_v_chan) == -EPROBE_DEFER)
		chg->iio.dcin_v_chan = iio_channel_get(chg->dev, "dcin_v");

	if (IS_ERR(chg->iio.dcin_v_chan))
		return PTR_ERR(chg->iio.dcin_v_chan);

	return iio_read_channel_processed(chg->iio.dcin_v_chan, &val->intval);
}

int smblib_get_prop_dc_current_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_get_prop_dc_present(chg, val);
	if (rc < 0 || !val->intval)
		return rc;

	if (!chg->iio.dcin_i_chan ||
		PTR_ERR(chg->iio.dcin_i_chan) == -EPROBE_DEFER)
		chg->iio.dcin_i_chan = iio_channel_get(chg->dev, "dcin_i");

	if (IS_ERR(chg->iio.dcin_i_chan))
		return PTR_ERR(chg->iio.dcin_i_chan);

	return iio_read_channel_processed(chg->iio.dcin_i_chan, &val->intval);
}
#endif
#endif

int smblib_get_prop_charger_temp(struct smb_charger *chg,
				 union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.temp_chan ||
		PTR_ERR(chg->iio.temp_chan) == -EPROBE_DEFER)
		chg->iio.temp_chan = iio_channel_get(chg->dev, "charger_temp");

	if (IS_ERR(chg->iio.temp_chan))
		return PTR_ERR(chg->iio.temp_chan);

	rc = iio_read_channel_processed(chg->iio.temp_chan, &val->intval);
	val->intval /= 100;
	return rc;
}

int smblib_get_prop_charger_temp_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.temp_max_chan ||
		PTR_ERR(chg->iio.temp_max_chan) == -EPROBE_DEFER)
		chg->iio.temp_max_chan = iio_channel_get(chg->dev,
							 "charger_temp_max");
	if (IS_ERR(chg->iio.temp_max_chan))
		return PTR_ERR(chg->iio.temp_max_chan);

	rc = iio_read_channel_processed(chg->iio.temp_max_chan, &val->intval);
	val->intval /= 100;
	return rc;
}

int smblib_get_prop_typec_cc_orientation(struct smb_charger *chg,
					 union power_supply_propval *val)
{
	if (chg->typec_status[3] & CC_ATTACHED_BIT)
		val->intval =
			(bool)(chg->typec_status[3] & CC_ORIENTATION_BIT) + 1;
	else
		val->intval = 0;

	return 0;
}

static const char * const smblib_typec_mode_name[] = {
	[POWER_SUPPLY_TYPEC_NONE]		  = "NONE",
	[POWER_SUPPLY_TYPEC_SOURCE_DEFAULT]	  = "SOURCE_DEFAULT",
	[POWER_SUPPLY_TYPEC_SOURCE_MEDIUM]	  = "SOURCE_MEDIUM",
	[POWER_SUPPLY_TYPEC_SOURCE_HIGH]	  = "SOURCE_HIGH",
	[POWER_SUPPLY_TYPEC_NON_COMPLIANT]	  = "NON_COMPLIANT",
	[POWER_SUPPLY_TYPEC_SINK]		  = "SINK",
	[POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE]   = "SINK_POWERED_CABLE",
	[POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY] = "SINK_DEBUG_ACCESSORY",
	[POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER]   = "SINK_AUDIO_ADAPTER",
	[POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY]   = "POWERED_CABLE_ONLY",
};

static int smblib_get_prop_ufp_mode(struct smb_charger *chg)
{
	switch (chg->typec_status[0]) {
	case UFP_TYPEC_RDSTD_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
	case UFP_TYPEC_RD1P5_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_MEDIUM;
	case UFP_TYPEC_RD3P0_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_HIGH;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_dfp_mode(struct smb_charger *chg)
{
	switch (chg->typec_status[1] & DFP_TYPEC_MASK) {
	case DFP_RA_RA_BIT:
		return POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER;
	case DFP_RD_RD_BIT:
		return POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY;
	case DFP_RD_RA_VCONN_BIT:
		return POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE;
	case DFP_RD_OPEN_BIT:
		return POWER_SUPPLY_TYPEC_SINK;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_typec_mode(struct smb_charger *chg)
{
	if (chg->typec_status[3] & UFP_DFP_MODE_STATUS_BIT)
		return smblib_get_prop_dfp_mode(chg);
	else
		return smblib_get_prop_ufp_mode(chg);
}

int smblib_get_prop_typec_power_role(struct smb_charger *chg,
				     union power_supply_propval *val)
{
	int rc = 0;
	u8 ctrl;

	rc = smblib_read(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG, &ctrl);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_INTRPT_ENB_SOFTWARE_CTRL = 0x%02x\n",
		   ctrl);

	if (ctrl & TYPEC_DISABLE_CMD_BIT) {
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		return rc;
	}

	switch (ctrl & (DFP_EN_CMD_BIT | UFP_EN_CMD_BIT)) {
	case 0:
		val->intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		break;
	case DFP_EN_CMD_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	case UFP_EN_CMD_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	default:
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		smblib_err(chg, "unsupported power role 0x%02lx\n",
			ctrl & (DFP_EN_CMD_BIT | UFP_EN_CMD_BIT));
		return -EINVAL;
	}

	return rc;
}

int smblib_get_prop_pd_allowed(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = get_effective_result(chg->pd_allowed_votable);
	return 0;
}

int smblib_get_prop_input_current_settled(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	return smblib_get_charge_param(chg, &chg->param.icl_stat, &val->intval);
}

#define HVDCP3_STEP_UV	200000
int smblib_get_prop_input_voltage_settled(struct smb_charger *chg,
						union power_supply_propval *val)
{
	int rc, pulses;

	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		rc = smblib_get_pulse_cnt(chg, &pulses);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			return 0;
		}
		val->intval = MICRO_5V + HVDCP3_STEP_UV * pulses;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_min_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_pd_in_hard_reset(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = chg->pd_hard_reset;
	return 0;
}

int smblib_get_pe_start(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	/*
	 * hvdcp timeout voter is the last one to allow pd. Use its vote
	 * to indicate start of pe engine
	 */
	val->intval
		= !get_client_vote_locked(chg->pd_disallowed_votable_indirect,
			HVDCP_TIMEOUT_VOTER);
	return 0;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
int smblib_get_prop_legacy_cable_status(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc;
	u8 reg;

	rc = smblib_read(chg, TYPE_C_STATUS_5_REG, &reg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_5 rc=%d\n", rc);
		return rc;
	}

	val->intval = (reg & TYPEC_LEGACY_CABLE_STATUS_BIT) ? 1 : 0;
	return 0;
}
#endif

int smblib_get_prop_die_health(struct smb_charger *chg,
						union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TEMP_RANGE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TEMP_RANGE_STATUS_REG rc=%d\n",
									rc);
		return rc;
	}

	if (stat & ALERT_LEVEL_BIT)
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (stat & TEMP_ABOVE_RANGE_BIT)
		val->intval = POWER_SUPPLY_HEALTH_HOT;
	else if (stat & TEMP_WITHIN_RANGE_BIT)
		val->intval = POWER_SUPPLY_HEALTH_WARM;
	else if (stat & TEMP_BELOW_RANGE_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COOL;
	else
		val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;

	return 0;
}

#define SDP_CURRENT_UA			500000
#define CDP_CURRENT_UA			1500000
#define DCP_CURRENT_UA			1500000
#define HVDCP_CURRENT_UA		3000000
#define TYPEC_DEFAULT_CURRENT_UA	900000
#define TYPEC_MEDIUM_CURRENT_UA		1500000
#define TYPEC_HIGH_CURRENT_UA		3000000
static int get_rp_based_dcp_current(struct smb_charger *chg, int typec_mode)
{
	int rp_ua;

	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		rp_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	/* fall through */
	default:
		rp_ua = DCP_CURRENT_UA;
	}

	return rp_ua;
}

/*******************
 * USB PSY SETTERS *
 * *****************/

int smblib_set_prop_pd_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	if (chg->pd_active)
		rc = vote(chg->usb_icl_votable, PD_VOTER, true, val->intval);
	else
		rc = -EPERM;

	return rc;
}

static int smblib_handle_usb_current(struct smb_charger *chg,
					int usb_current)
{
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	int rc = 0;

	if (usb_current == -ETIMEDOUT)
		return 0;

	switch (usb_current) {
	case USBIN_100MA:
	case USBIN_150MA:
	case USBIN_500MA:
		usb_current -= USBIN_25MA;
		break;
	case USBIN_900MA:
		usb_current -= USBIN_50MA;
		break;
	default:
		break;
	}
	rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, true, usb_current);

	/* Changing ICL by this function may change online */
	power_supply_changed(chg->usb_psy);
#else
	int rc = 0, rp_ua, typec_mode;

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_FLOAT) {
		if (usb_current == -ETIMEDOUT) {
			/*
			 * Valid FLOAT charger, report the current based
			 * of Rp
			 */
			typec_mode = smblib_get_prop_typec_mode(chg);
			rp_ua = get_rp_based_dcp_current(chg, typec_mode);
			rc = vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER,
								true, rp_ua);
			if (rc < 0)
				return rc;
		} else {
			/*
			 * FLOAT charger detected as SDP by USB driver,
			 * charge with the requested current and update the
			 * real_charger_type
			 */
			chg->real_charger_type = POWER_SUPPLY_TYPE_USB;
			rc = vote(chg->usb_icl_votable, USB_PSY_VOTER,
						true, usb_current);
			if (rc < 0)
				return rc;
			rc = vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER,
							false, 0);
			if (rc < 0)
				return rc;
		}
	} else if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB &&
		usb_current == -ETIMEDOUT) {
		rc = vote(chg->usb_icl_votable, USB_PSY_VOTER,
					true, USBIN_100MA);
	} else {
		rc = vote(chg->usb_icl_votable, USB_PSY_VOTER,
					true, usb_current);
	}
#endif

	return rc;
}

int smblib_set_prop_sdp_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc = 0;

	if (!chg->pd_active) {
		rc = smblib_handle_usb_current(chg, val->intval);
	} else if (chg->system_suspend_supported) {
		if (val->intval <= USBIN_25MA)
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, true, val->intval);
		else
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, false, 0);
	}
	return rc;
}

int smblib_set_prop_boost_current(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_boost,
				val->intval <= chg->boost_threshold_ua ?
				chg->chg_freq.freq_below_otg_threshold :
				chg->chg_freq.freq_above_otg_threshold);
	if (rc < 0) {
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);
		return rc;
	}

	chg->boost_current_ua = val->intval;
	return rc;
}

int smblib_set_prop_typec_power_role(struct smb_charger *chg,
				     const union power_supply_propval *val)
{
	/* Check if power role switch is disabled */
	if (!get_effective_result(chg->disable_power_role_switch))
		return __smblib_set_prop_typec_power_role(chg, val);

	return 0;
}

int smblib_set_prop_typec_select_rp(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc = 0;

	if (!typec_in_src_mode(chg)) {
		smblib_err(chg, "Couldn't set curr src: not in SRC mode\n");
		return -EINVAL;
	}

	if (val->intval < 0 || val->intval >= TYPEC_SRC_RP_MAX_ELEMENTS)
		return -EINVAL;

	switch (val->intval) {
	case TYPEC_SRC_RP_STD:
		rc = smblib_masked_write(chg, TYPE_C_CFG_2_REG,
			EN_80UA_180UA_CUR_SOURCE_BIT,
			TYPEC_SRC_RP_STD);
		break;
	case TYPEC_SRC_RP_1P5A:
	case TYPEC_SRC_RP_3A:
	case TYPEC_SRC_RP_3A_DUPLICATE:
		rc = smblib_masked_write(chg, TYPE_C_CFG_2_REG,
			EN_80UA_180UA_CUR_SOURCE_BIT,
			TYPEC_SRC_RP_1P5A);
		break;
	default:
		return -EINVAL;
	}

	if (rc < 0)
		smblib_err(chg, "Couldn't write to TYPE_C_CURRSRC_CFG rc=%d\n",
				rc);
	return rc;
}

int smblib_set_prop_pd_voltage_min(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, min_uv;

	min_uv = min(val->intval, chg->voltage_max_uv);
	rc = smblib_set_usb_pd_allowed_voltage(chg, min_uv,
					       chg->voltage_max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid max voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_min_uv = min_uv;
	power_supply_changed(chg->usb_main_psy);
	return rc;
}

int smblib_set_prop_pd_voltage_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, max_uv;

	max_uv = max(val->intval, chg->voltage_min_uv);
	rc = smblib_set_usb_pd_allowed_voltage(chg, chg->voltage_min_uv,
					       max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid min voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_max_uv = max_uv;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (chg->voltage_max_uv > MICRO_5V)
		vote(chg->usb_icl_votable, HIGH_VOLTAGE_VOTER,
					true, chg->high_voltage_icl_ua);
	else
		vote(chg->usb_icl_votable, HIGH_VOLTAGE_VOTER,
					false, 0);
	smblib_somc_thermal_icl_change(chg);
#endif
	return rc;
}

static int __smblib_set_prop_pd_active(struct smb_charger *chg, bool pd_active)
{
	int rc;
	bool orientation, sink_attached, hvdcp;
	u8 stat;

	chg->pd_active = pd_active;
	if (chg->pd_active) {
		chg->real_charger_type = POWER_SUPPLY_TYPE_USB_PD;
		vote(chg->apsd_disable_votable, PD_VOTER, true, 0);
		vote(chg->pd_allowed_votable, PD_VOTER, true, 0);
		vote(chg->usb_irq_enable_votable, PD_VOTER, true, 0);

		/*
		 * VCONN_EN_ORIENTATION_BIT controls whether to use CC1 or CC2
		 * line when TYPEC_SPARE_CFG_BIT (CC pin selection s/w override)
		 * is set or when VCONN_EN_VALUE_BIT is set.
		 */
		orientation = chg->typec_status[3] & CC_ORIENTATION_BIT;
		rc = smblib_masked_write(chg,
				TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				VCONN_EN_ORIENTATION_BIT,
				orientation ? 0 : VCONN_EN_ORIENTATION_BIT);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't enable vconn on CC line rc=%d\n", rc);

		/* SW controlled CC_OUT */
		rc = smblib_masked_write(chg, TAPER_TIMER_SEL_CFG_REG,
				TYPEC_SPARE_CFG_BIT, TYPEC_SPARE_CFG_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable SW cc_out rc=%d\n",
									rc);

		/*
		 * Enforce 500mA for PD until the real vote comes in later.
		 * It is guaranteed that pd_active is set prior to
		 * pd_current_max
		 */
		rc = vote(chg->usb_icl_votable, PD_VOTER, true, USBIN_500MA);
		if (rc < 0)
			smblib_err(chg, "Couldn't vote for USB ICL rc=%d\n",
									rc);

		/* since PD was found the cable must be non-legacy */
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, false, 0);

		/* clear USB ICL vote for DCP_VOTER */
		rc = vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't un-vote DCP from USB ICL rc=%d\n",
									rc);

		/* remove USB_PSY_VOTER */
		rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't unvote USB_PSY rc=%d\n", rc);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
		determine_charger_type(chg);
#endif
	} else {
		rc = smblib_read(chg, APSD_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read APSD status rc=%d\n",
									rc);
			return rc;
		}

		hvdcp = stat & QC_CHARGER_BIT;
		vote(chg->apsd_disable_votable, PD_VOTER, false, 0);
		vote(chg->pd_allowed_votable, PD_VOTER, false, 0);
		vote(chg->usb_irq_enable_votable, PD_VOTER, false, 0);
		vote(chg->hvdcp_disable_votable_indirect, PD_INACTIVE_VOTER,
								false, 0);

		/* HW controlled CC_OUT */
		rc = smblib_masked_write(chg, TAPER_TIMER_SEL_CFG_REG,
							TYPEC_SPARE_CFG_BIT, 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable HW cc_out rc=%d\n",
									rc);

		/*
		 * This WA should only run for HVDCP. Non-legacy SDP/CDP could
		 * draw more, but this WA will remove Rd causing VBUS to drop,
		 * and data could be interrupted. Non-legacy DCP could also draw
		 * more, but it may impact compliance.
		 */
		sink_attached = chg->typec_status[3] & UFP_DFP_MODE_STATUS_BIT;
		if ((chg->connector_type != POWER_SUPPLY_CONNECTOR_MICRO_USB)
				&& !chg->typec_legacy_valid
				&& !sink_attached && hvdcp)
			schedule_work(&chg->legacy_detection_work);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
		else
			determine_charger_type(chg);
#endif
	}

	smblib_update_usb_type(chg);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_somc_thermal_icl_change(chg);
	smblib_dbg(chg, PR_SOMC, "%s PD\n",
				chg->pd_active ? "enabling" : "disabling");
#endif
	power_supply_changed(chg->usb_psy);
	return rc;
}

int smblib_set_prop_pd_active(struct smb_charger *chg,
			      const union power_supply_propval *val)
{
	if (!get_effective_result(chg->pd_allowed_votable))
		return -EINVAL;

	return __smblib_set_prop_pd_active(chg, val->intval);
}

int smblib_set_prop_ship_mode(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "Set ship mode: %d!!\n", !!val->intval);

	rc = smblib_masked_write(chg, SHIP_MODE_REG, SHIP_MODE_EN_BIT,
			!!val->intval ? SHIP_MODE_EN_BIT : 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't %s ship mode, rc=%d\n",
				!!val->intval ? "enable" : "disable", rc);

	return rc;
}

int smblib_reg_block_update(struct smb_charger *chg,
				struct reg_info *entry)
{
	int rc = 0;

	while (entry && entry->reg) {
		rc = smblib_read(chg, entry->reg, &entry->bak);
		if (rc < 0) {
			dev_err(chg->dev, "Error in reading %s rc=%d\n",
				entry->desc, rc);
			break;
		}
		entry->bak &= entry->mask;

		rc = smblib_masked_write(chg, entry->reg,
					 entry->mask, entry->val);
		if (rc < 0) {
			dev_err(chg->dev, "Error in writing %s rc=%d\n",
				entry->desc, rc);
			break;
		}
		entry++;
	}

	return rc;
}

int smblib_reg_block_restore(struct smb_charger *chg,
				struct reg_info *entry)
{
	int rc = 0;

	while (entry && entry->reg) {
		rc = smblib_masked_write(chg, entry->reg,
					 entry->mask, entry->bak);
		if (rc < 0) {
			dev_err(chg->dev, "Error in writing %s rc=%d\n",
				entry->desc, rc);
			break;
		}
		entry++;
	}

	return rc;
}

static struct reg_info cc2_detach_settings[] = {
	{
		.reg	= TYPE_C_CFG_2_REG,
		.mask	= TYPE_C_UFP_MODE_BIT | EN_TRY_SOURCE_MODE_BIT,
		.val	= TYPE_C_UFP_MODE_BIT,
		.desc	= "TYPE_C_CFG_2_REG",
	},
	{
		.reg	= TYPE_C_CFG_3_REG,
		.mask	= EN_TRYSINK_MODE_BIT,
		.val	= 0,
		.desc	= "TYPE_C_CFG_3_REG",
	},
	{
		.reg	= TAPER_TIMER_SEL_CFG_REG,
		.mask	= TYPEC_SPARE_CFG_BIT,
		.val	= TYPEC_SPARE_CFG_BIT,
		.desc	= "TAPER_TIMER_SEL_CFG_REG",
	},
	{
		.reg	= TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
		.mask	= VCONN_EN_ORIENTATION_BIT,
		.val	= 0,
		.desc	= "TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG",
	},
	{
		.reg	= MISC_CFG_REG,
		.mask	= TCC_DEBOUNCE_20MS_BIT,
		.val	= TCC_DEBOUNCE_20MS_BIT,
		.desc	= "Tccdebounce time"
	},
	{
	},
};

static int smblib_cc2_sink_removal_enter(struct smb_charger *chg)
{
	int rc, ccout, ufp_mode;
	u8 stat;

	if ((chg->wa_flags & TYPEC_CC2_REMOVAL_WA_BIT) == 0)
		return 0;

	if (chg->cc2_detach_wa_active)
		return 0;

	rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}

	ccout = (stat & CC_ATTACHED_BIT) ?
					(!!(stat & CC_ORIENTATION_BIT) + 1) : 0;
	ufp_mode = (stat & TYPEC_DEBOUNCE_DONE_STATUS_BIT) ?
					!(stat & UFP_DFP_MODE_STATUS_BIT) : 0;

	if (ccout != 2)
		return 0;

	if (!ufp_mode)
		return 0;

	chg->cc2_detach_wa_active = true;
	/* The CC2 removal WA will cause a type-c-change IRQ storm */
	smblib_reg_block_update(chg, cc2_detach_settings);
	schedule_work(&chg->rdstd_cc2_detach_work);
	return rc;
}

static int smblib_cc2_sink_removal_exit(struct smb_charger *chg)
{
	if ((chg->wa_flags & TYPEC_CC2_REMOVAL_WA_BIT) == 0)
		return 0;

	if (!chg->cc2_detach_wa_active)
		return 0;

	chg->cc2_detach_wa_active = false;
	chg->in_chg_lock = true;
	cancel_work_sync(&chg->rdstd_cc2_detach_work);
	chg->in_chg_lock = false;
	smblib_reg_block_restore(chg, cc2_detach_settings);
	return 0;
}

int smblib_set_prop_pd_in_hard_reset(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc = 0;

	if (chg->pd_hard_reset == val->intval)
		return rc;

	chg->pd_hard_reset = val->intval;
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			EXIT_SNK_BASED_ON_CC_BIT,
			(chg->pd_hard_reset) ? EXIT_SNK_BASED_ON_CC_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set EXIT_SNK_BASED_ON_CC rc=%d\n",
				rc);

	vote(chg->apsd_disable_votable, PD_HARD_RESET_VOTER,
							chg->pd_hard_reset, 0);

	return rc;
}

static int smblib_recover_from_soft_jeita(struct smb_charger *chg)
{
	u8 stat_1, stat_2;
	int rc;

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->jeita_sw_ctl_en)
		return 0;
#endif

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat_1);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return rc;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat_2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
		return rc;
	}

	if ((chg->jeita_status && !(stat_2 & BAT_TEMP_STATUS_SOFT_LIMIT_MASK) &&
		((stat_1 & BATTERY_CHARGER_STATUS_MASK) == TERMINATE_CHARGE))) {
		/*
		 * We are moving from JEITA soft -> Normal and charging
		 * is terminated
		 */
		rc = smblib_write(chg, CHARGING_ENABLE_CMD_REG, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable charging rc=%d\n",
						rc);
			return rc;
		}
		rc = smblib_write(chg, CHARGING_ENABLE_CMD_REG,
						CHARGING_ENABLE_CMD_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable charging rc=%d\n",
						rc);
			return rc;
		}
	}

	chg->jeita_status = stat_2 & BAT_TEMP_STATUS_SOFT_LIMIT_MASK;

	return 0;
}

/************************
 * USB MAIN PSY GETTERS *
 ************************/
int smblib_get_prop_fcc_delta(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc, jeita_cc_delta_ua = 0;

	if (chg->sw_jeita_enabled) {
		val->intval = 0;
		return 0;
	}

	rc = smblib_get_jeita_cc_delta(chg, &jeita_cc_delta_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get jeita cc delta rc=%d\n", rc);
		jeita_cc_delta_ua = 0;
	}

	val->intval = jeita_cc_delta_ua;
	return 0;
}

/************************
 * USB MAIN PSY SETTERS *
 ************************/
int smblib_get_charge_current(struct smb_charger *chg,
				int *total_current_ua)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);
	union power_supply_propval val = {0, };
	int rc = 0, typec_source_rd, current_ua;
	bool non_compliant;
	u8 stat5;

	if (chg->pd_active) {
		*total_current_ua =
			get_client_vote_locked(chg->usb_icl_votable, PD_VOTER);
		return rc;
	}

	rc = smblib_read(chg, TYPE_C_STATUS_5_REG, &stat5);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_5 rc=%d\n", rc);
		return rc;
	}
	non_compliant = stat5 & TYPEC_NONCOMP_LEGACY_CABLE_STATUS_BIT;

	/* get settled ICL */
	rc = smblib_get_prop_input_current_settled(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get settled ICL rc=%d\n", rc);
		return rc;
	}

	typec_source_rd = smblib_get_prop_ufp_mode(chg);

	/* QC 2.0/3.0 adapter */
	if (apsd_result->bit & (QC_3P0_BIT | QC_2P0_BIT)) {
		*total_current_ua = HVDCP_CURRENT_UA;
		return 0;
	}

	if (non_compliant) {
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = DCP_CURRENT_UA;
			break;
		default:
			current_ua = 0;
			break;
		}

		*total_current_ua = max(current_ua, val.intval);
		return 0;
	}

	switch (typec_source_rd) {
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = chg->default_icl_ua;
			break;
		default:
			current_ua = 0;
			break;
		}
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		current_ua = TYPEC_MEDIUM_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		current_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
	case POWER_SUPPLY_TYPEC_NONE:
	default:
		current_ua = 0;
		break;
	}

	*total_current_ua = max(current_ua, val.intval);
	return 0;
}

/************************
 * PARALLEL PSY GETTERS *
 ************************/

int smblib_get_prop_slave_current_now(struct smb_charger *chg,
		union power_supply_propval *pval)
{
	if (IS_ERR_OR_NULL(chg->iio.batt_i_chan))
		chg->iio.batt_i_chan = iio_channel_get(chg->dev, "batt_i");

	if (IS_ERR(chg->iio.batt_i_chan))
		return PTR_ERR(chg->iio.batt_i_chan);

	return iio_read_channel_processed(chg->iio.batt_i_chan, &pval->intval);
}

/**********************
 * INTERRUPT HANDLERS *
 **********************/

irqreturn_t smblib_handle_debug(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	return IRQ_HANDLED;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
irqreturn_t smblib_handle_dcin_debug(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;
	u8 int_rt_sts, aicl_sts;

	/* Get dc_int_rt_sts to log */
	rc = smblib_read(chg, 0x1410, &int_rt_sts);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DC_INT_RT_STS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	/* Read AICL_STATUS to log */
	rc = smblib_read(chg, AICL_STATUS_REG, &aicl_sts);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	smblib_dbg(chg, PR_SOMC,
			"IRQ: %s, dc_int_rt_sts: 0x%02x, aicl_status: 0x%02x\n",
			irq_data->name, int_rt_sts, aicl_sts);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_dcin_uv(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;
	u8 reg = 0;
	bool dcin_uv;

	rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &reg);
	smblib_dbg(chg, PR_SOMC, "IRQ: %s reg: 0x%02x\n", irq_data->name, reg);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read DC_INT_RT_STS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	dcin_uv = (bool)(reg & DCIN_UV_RT_STS_BIT);

	if (dcin_uv && !chg->dcin_uv)
		schedule_work(&chg->wireless_wa_fake_charging_work);

	chg->dcin_uv = dcin_uv;
	return IRQ_HANDLED;
}
#endif

irqreturn_t smblib_handle_otg_overcurrent(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;
	u8 stat;

	rc = smblib_read(chg, OTG_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read OTG_INT_RT_STS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	if (chg->wa_flags & OTG_WA) {
		schedule_work(&chg->ocp_otg_wa_work);

		if (stat & OTG_OC_DIS_SW_STS_RT_STS_BIT)
			smblib_err(chg, "OTG disabled by hw\n");

		/* not handling software based hiccups for PM660 */
		return IRQ_HANDLED;
	}

	if (stat & OTG_OVERCURRENT_RT_STS_BIT)
		schedule_work(&chg->otg_oc_work);

	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_chg_state_change(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_dbg(chg, PR_SOMC, "current charge state: %s\n",
				smblib_somc_get_battery_charger_status(chg));
#endif
	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_batt_temp_changed(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	rc = smblib_recover_from_soft_jeita(chg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't recover chg from soft jeita rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_dbg(chg, PR_SOMC, "IRQ: %s\n", irq_data->name);
#if defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->jeita_sw_ctl_en) {
		cancel_delayed_work_sync(&chg->jeita_work);
		schedule_delayed_work(&chg->jeita_work, msecs_to_jiffies(0));
	}
#endif
#endif
	rerun_election(chg->fcc_votable);
	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_batt_psy_changed(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_usb_psy_changed(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	power_supply_changed(chg->usb_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_usbin_uv(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#if defined(CONFIG_ARCH_SONY_TAMA)
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);
#else
	const struct apsd_result *apsd_result = smblib_update_usb_type(chg);
#endif
#else
	int rc;
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);
	u8 stat = 0, max_pulses = 0;
#endif

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	/* ESD workaround */
	if (!chg->typec_en_dis_active && (apsd_result->bit & QC_2P0_BIT)) {
		smblib_dbg(chg, PR_SOMC, "rerun APSD for ESD WA\n");
		smblib_masked_write(chg, CMD_APSD_REG, APSD_RERUN_BIT,
							APSD_RERUN_BIT);
		return IRQ_HANDLED;
	}
#endif
	if (!chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data)
		return IRQ_HANDLED;

	wdata = &chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data->storm_data;
	reset_storm_count(wdata);

#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (!chg->non_compliant_chg_detected &&
			apsd->pst == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read CHANGE_STATUS_REG rc=%d\n", rc);

		if (stat & QC_5V_BIT)
			return IRQ_HANDLED;

		rc = smblib_read(chg, HVDCP_PULSE_COUNT_MAX_REG, &max_pulses);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read QC2 max pulses rc=%d\n", rc);

		chg->non_compliant_chg_detected = true;
		chg->qc2_max_pulses = (max_pulses &
				HVDCP_PULSE_COUNT_MAX_QC2_MASK);

		if (stat & QC_12V_BIT) {
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_9V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 9V rc=%d\n",
						rc);

		} else if (stat & QC_9V_BIT) {
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_5V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 5V rc=%d\n",
						rc);

		}

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				0);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn off SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		smblib_rerun_apsd(chg);
	}
#endif

	return IRQ_HANDLED;
}

static void smblib_micro_usb_plugin(struct smb_charger *chg, bool vbus_rising)
{
	if (vbus_rising) {
		/* use the typec flag even though its not typec */
		chg->typec_present = true;
	} else {
		chg->typec_present = false;
		smblib_update_usb_type(chg);
		extcon_set_state_sync(chg->extcon, EXTCON_USB, false);
		smblib_uusb_removal(chg);
	}
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
static int smblib_somc_report_wireless_thermal_limit(struct smb_charger *chg,
								int max_voltage)
{
	int rc;
	union power_supply_propval pval = {0, };

	if (!chg->wireless_enable)
		return -EINVAL;

	if (!chg->wireless_psy)
		chg->wireless_psy = power_supply_get_by_name("wireless");

	if (!chg->wireless_psy) {
		smblib_err(chg,
			   "Couldn't set property: WIRELESS_THERMAL_V_LIMIT\n");
		return -EINVAL;
	}

	pval.intval = max_voltage;
	rc = power_supply_set_property(chg->wireless_psy,
				POWER_SUPPLY_PROP_WIRELESS_THERMAL_V_LIMIT,
				&pval);
	return rc;
}
#endif

void smblib_usb_plugin_hard_reset_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	if (vbus_rising) {
		/* Remove FCC_STEPPER 1.5A init vote to allow FCC ramp up */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);

		smblib_cc2_sink_removal_exit(chg);
	} else {
		/* Force 1500mA FCC on USB removal if fcc stepper is enabled */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
							true, 1500000);

		smblib_cc2_sink_removal_enter(chg);
		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}
	}

	power_supply_changed(chg->usb_psy);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
}

#define PL_DELAY_MS			30000
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#define REMOVAL_DELAY_MS		2000
#define REMOVAL_WAKE_PERIOD		(3 * HZ)
#endif
void smblib_usb_plugin_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	smblib_set_opt_freq_buck(chg, vbus_rising ? chg->chg_freq.freq_5V :
						chg->chg_freq.freq_removal);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->jeita_sw_ctl_en) {
		cancel_delayed_work_sync(&chg->jeita_work);
		schedule_delayed_work(&chg->jeita_work, msecs_to_jiffies(0));
	}
#endif

	if (vbus_rising) {
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
#if defined(CONFIG_ARCH_SONY_TAMA)
		if (chg->wireless_enable)
			vote(chg->dc_icl_votable, DC_OV_BY_PLUGIN_VOTER,
								true, 0);
#endif

		smblib_cc2_sink_removal_exit(chg);

		if (chg->typec_en_dis_active) {
			smblib_dbg(chg, PR_SOMC,
				"start fake charging by typec_en_dis_active\n");
			schedule_work(&chg->fake_charging_work);
		}
#else
		if (smblib_get_prop_dfp_mode(chg) != POWER_SUPPLY_TYPEC_NONE) {
			chg->fake_usb_insertion = true;
			return;
		}
#endif

		rc = smblib_request_dpdm(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);

		/* Remove FCC_STEPPER 1.5A init vote to allow FCC ramp up */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);

		/* Schedule work to enable parallel charger */
		vote(chg->awake_votable, PL_DELAY_VOTER, true, 0);
		schedule_delayed_work(&chg->pl_enable_work,
					msecs_to_jiffies(PL_DELAY_MS));
		/* vbus rising when APSD was disabled and PD_ACTIVE = 0 */
		if (get_effective_result(chg->apsd_disable_votable) &&
				!chg->pd_active)
			pr_err("APSD disabled on vbus rising without PD\n");
	} else {
#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
		if (chg->fake_usb_insertion) {
			chg->fake_usb_insertion = false;
			return;
		}
#endif

		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}

		/* Force 1500mA FCC on removal if fcc stepper is enabled */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
							true, 1500000);

		rc = smblib_request_dpdm(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
		smblib_somc_lrc_check(chg);
#if defined(CONFIG_ARCH_SONY_TAMA)
		if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER)
			vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER,
								true, 0);

#endif
		__pm_wakeup_event(
			chg->usb_removal_wakelock.lock, REMOVAL_WAKE_PERIOD);
		schedule_delayed_work(&chg->usb_removal_work,
					msecs_to_jiffies(REMOVAL_DELAY_MS));
		if (chg->pd_hard_reset)
			smblib_cc2_sink_removal_enter(chg);

#if defined(CONFIG_ARCH_SONY_TAMA)
		if (chg->wireless_enable) {
			vote(chg->dc_icl_votable, DC_OV_BY_PLUGIN_VOTER,
								false, 0);
			smblib_somc_handle_wireless_exclusion(chg);
		}
#endif
#endif
	}

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		smblib_micro_usb_plugin(chg, vbus_rising);

	power_supply_changed(chg->usb_psy);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
}

irqreturn_t smblib_handle_usb_plugin(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	mutex_lock(&chg->lock);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_somc_handle_profile_fv_rb(chg, false);
#else
	if (chg->pd_hard_reset)
		smblib_usb_plugin_hard_reset_locked(chg);
	else
#endif
		smblib_usb_plugin_locked(chg);
	mutex_unlock(&chg->lock);
	return IRQ_HANDLED;
}

#define USB_WEAK_INPUT_UA	1400000
#define ICL_CHANGE_DELAY_MS	1000
irqreturn_t smblib_handle_icl_change(int irq, void *data)
{
	u8 stat;
	int rc, settled_ua, delay = ICL_CHANGE_DELAY_MS;
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->mode == PARALLEL_MASTER) {
		rc = smblib_read(chg, AICL_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n",
					rc);
			return IRQ_HANDLED;
		}

		rc = smblib_get_charge_param(chg, &chg->param.icl_stat,
				&settled_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
			return IRQ_HANDLED;
		}

		/* If AICL settled then schedule work now */
		if ((settled_ua == get_effective_result(chg->usb_icl_votable))
				|| (stat & AICL_DONE_BIT))
			delay = 0;

		cancel_delayed_work_sync(&chg->icl_change_work);
		schedule_delayed_work(&chg->icl_change_work,
						msecs_to_jiffies(delay));
	}

	return IRQ_HANDLED;
}

static void smblib_handle_slow_plugin_timeout(struct smb_charger *chg,
					      bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: slow-plugin-timeout %s\n",
		   rising ? "rising" : "falling");
}

static void smblib_handle_sdp_enumeration_done(struct smb_charger *chg,
					       bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: sdp-enumeration-done %s\n",
		   rising ? "rising" : "falling");
}

#define MICRO_10P3V	10300000
static void smblib_check_ov_condition(struct smb_charger *chg)
{
	union power_supply_propval pval = {0, };
	int rc;

	if (chg->wa_flags & OV_IRQ_WA_BIT) {
		rc = power_supply_get_property(chg->usb_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get current voltage, rc=%d\n",
				rc);
			return;
		}

		if (pval.intval > MICRO_10P3V) {
			smblib_err(chg, "USBIN OV detected\n");
			vote(chg->hvdcp_hw_inov_dis_votable, OV_VOTER, true,
				0);
			pval.intval = POWER_SUPPLY_DP_DM_FORCE_5V;
			rc = power_supply_set_property(chg->batt_psy,
				POWER_SUPPLY_PROP_DP_DM, &pval);
			return;
		}
	}
}

#define QC3_PULSES_FOR_6V	5
#define QC3_PULSES_FOR_9V	20
#define QC3_PULSES_FOR_12V	35
static void smblib_hvdcp_adaptive_voltage_change(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	int pulses;

	smblib_check_ov_condition(chg);
	power_supply_changed(chg->usb_main_psy);
	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_CHANGE_STATUS rc=%d\n", rc);
			return;
		}

		switch (stat & QC_2P0_STATUS_MASK) {
		case QC_5V_BIT:
			smblib_set_opt_freq_buck(chg,
					chg->chg_freq.freq_5V);
			break;
		case QC_9V_BIT:
			smblib_set_opt_freq_buck(chg,
					chg->chg_freq.freq_9V);
			vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
			break;
		case QC_12V_BIT:
			smblib_set_opt_freq_buck(chg,
					chg->chg_freq.freq_12V);
			vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
			break;
		default:
			smblib_set_opt_freq_buck(chg,
					chg->chg_freq.freq_removal);
			break;
		}
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
		rc = smblib_get_pulse_cnt(chg, &pulses);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			return;
		}

		if (pulses < QC3_PULSES_FOR_6V)
			smblib_set_opt_freq_buck(chg,
				chg->chg_freq.freq_5V);
		else if (pulses < QC3_PULSES_FOR_9V)
			smblib_set_opt_freq_buck(chg,
				chg->chg_freq.freq_6V_8V);
		else if (pulses < QC3_PULSES_FOR_12V)
			smblib_set_opt_freq_buck(chg,
				chg->chg_freq.freq_9V);
		else
			smblib_set_opt_freq_buck(chg,
				chg->chg_freq.freq_12V);
	}
}

/* triggers when HVDCP 3.0 authentication has finished */
static void smblib_handle_hvdcp_3p0_auth_done(struct smb_charger *chg,
					      bool rising)
{
	const struct apsd_result *apsd_result;
	int rc;

	if (!rising)
		return;

	if (chg->wa_flags & QC_AUTH_INTERRUPT_WA_BIT) {
		/*
		 * Disable AUTH_IRQ_EN_CFG_BIT to receive adapter voltage
		 * change interrupt.
		 */
		rc = smblib_masked_write(chg,
				USBIN_SOURCE_CHANGE_INTRPT_ENB_REG,
				AUTH_IRQ_EN_CFG_BIT, 0);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't enable QC auth setting rc=%d\n", rc);
	}

	if (chg->mode == PARALLEL_MASTER)
		vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, true, 0);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	determine_charger_type(chg);
#endif

	/* the APSD done handler will set the USB supply type */
	apsd_result = smblib_get_apsd_result(chg);

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-3p0-auth-done rising; %s detected\n",
		   apsd_result->name);
}

static void smblib_handle_hvdcp_check_timeout(struct smb_charger *chg,
					      bool rising, bool qc_charger)
{
#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);
#endif

	/* Hold off PD only until hvdcp 2.0 detection timeout */
	if (rising) {
		vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
								false, 0);

		/* enable HDC and ICL irq for QC2/3 charger */
		if (qc_charger)
			vote(chg->usb_irq_enable_votable, QC_VOTER, true, 0);

#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
		/*
		 * HVDCP detection timeout done
		 * If adapter is not QC2.0/QC3.0 - it is a plain old DCP.
		 */
		if (!qc_charger && (apsd_result->bit & DCP_CHARGER_BIT))
			/* enforce DCP ICL if specified */
			vote(chg->usb_icl_votable, DCP_VOTER,
				chg->dcp_icl_ua != -EINVAL, chg->dcp_icl_ua);
#endif

		/*
		 * if pd is not allowed, then set pd_active = false right here,
		 * so that it starts the hvdcp engine
		 */
		if (!get_effective_result(chg->pd_allowed_votable))
			__smblib_set_prop_pd_active(chg, 0);
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp_check_timeout %s\n",
		   rising ? "rising" : "falling");
}

/* triggers when HVDCP is detected */
static void smblib_handle_hvdcp_detect_done(struct smb_charger *chg,
					    bool rising)
{
	if (!rising)
		return;

	/* the APSD done handler will set the USB supply type */
	cancel_delayed_work_sync(&chg->hvdcp_detect_work);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-detect-done %s\n",
		   rising ? "rising" : "falling");
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
static void smblib_somc_force_legacy_icl(struct smb_charger *chg,
							u8 apsd_result_bit)
{
	int icl_ua = 0;

	/* while PD is active it should have complete ICL control */
	if (chg->pd_active) {
		smblib_dbg(chg, PR_SOMC,
			"PD is active, does not set ICL by APSD result\n");
		return;
	}

#if !defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER) {
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true,
								USBIN_500MA);
		return;
	}
#endif

	switch (apsd_result_bit) {
	case SDP_CHARGER_BIT:
		if (!is_client_vote_enabled(chg->usb_icl_votable,
								USB_PSY_VOTER))
			vote(chg->usb_icl_votable, USB_PSY_VOTER, true, 0);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, false, 0);
		break;
	case CDP_CHARGER_BIT:
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true,
								USBIN_1500MA);
		break;
	case DCP_CHARGER_BIT:
		if (chg->typec_status[4] & TYPEC_LEGACY_CABLE_STATUS_BIT)
			icl_ua = chg->dcp_icl_ua != -EINVAL ?
						chg->dcp_icl_ua : USBIN_1500MA;
		else
			icl_ua = get_rp_based_dcp_current(chg, chg->typec_mode);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, icl_ua);
		break;
	case OCP_CHARGER_BIT:
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true,
								USBIN_1000MA);
		break;
	case FLOAT_CHARGER_BIT:
		switch (chg->typec_mode) {
		case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
			if (chg->typec_status[4] &
						TYPEC_LEGACY_CABLE_STATUS_BIT)
				icl_ua = USBIN_500MA;
			else
				icl_ua = get_rp_based_dcp_current(chg,
							chg->typec_mode);
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
			if (chg->typec_status[4] &
						TYPEC_LEGACY_CABLE_STATUS_BIT)
				icl_ua = USBIN_1500MA;
			else
				icl_ua = get_rp_based_dcp_current(chg,
							chg->typec_mode);
			break;
		case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
			icl_ua = USBIN_500MA;
			break;
		default:
			icl_ua = 0;
			break;
		}
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, icl_ua);
		break;
	case 0:
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 0);
		break;
	default:
		break;
	}
}
#else
static void smblib_force_legacy_icl(struct smb_charger *chg, int pst)
{
	int typec_mode;
	int rp_ua;

	/* while PD is active it should have complete ICL control */
	if (chg->pd_active)
		return;

	switch (pst) {
	case POWER_SUPPLY_TYPE_USB:
		/*
		 * USB_PSY will vote to increase the current to 500/900mA once
		 * enumeration is done. Ensure that USB_PSY has at least voted
		 * for 100mA before releasing the LEGACY_UNKNOWN vote
		 */
		if (!is_client_vote_enabled(chg->usb_icl_votable,
								USB_PSY_VOTER))
			vote(chg->usb_icl_votable, USB_PSY_VOTER, true, 100000);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, false, 0);
		break;
	case POWER_SUPPLY_TYPE_USB_CDP:
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 1500000);
		break;
	case POWER_SUPPLY_TYPE_USB_DCP:
		typec_mode = smblib_get_prop_typec_mode(chg);
		rp_ua = get_rp_based_dcp_current(chg, typec_mode);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, rp_ua);
		break;
	case POWER_SUPPLY_TYPE_USB_FLOAT:
		/*
		 * limit ICL to 100mA, the USB driver will enumerate to check
		 * if this is a SDP and appropriately set the current
		 */
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 100000);
		break;
	case POWER_SUPPLY_TYPE_USB_HVDCP:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 3000000);
		break;
	default:
		smblib_err(chg, "Unknown APSD %d; forcing 500mA\n", pst);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 500000);
		break;
	}
}
#endif

static void smblib_notify_extcon_props(struct smb_charger *chg, int id)
{
	union extcon_property_value val;
	union power_supply_propval prop_val;

	smblib_get_prop_typec_cc_orientation(chg, &prop_val);
	val.intval = ((prop_val.intval == 2) ? 1 : 0);
	extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_TYPEC_POLARITY, val);

	val.intval = true;
	extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_SS, val);
}

static void smblib_notify_device_mode(struct smb_charger *chg, bool enable)
{
	if (enable)
		smblib_notify_extcon_props(chg, EXTCON_USB);

	extcon_set_state_sync(chg->extcon, EXTCON_USB, enable);
}

static void smblib_notify_usb_host(struct smb_charger *chg, bool enable)
{
	if (enable)
		smblib_notify_extcon_props(chg, EXTCON_USB_HOST);

	extcon_set_state_sync(chg->extcon, EXTCON_USB_HOST, enable);
}

#define HVDCP_DET_MS 2500
static void smblib_handle_apsd_done(struct smb_charger *chg, bool rising)
{
	const struct apsd_result *apsd_result;

	if (!rising)
		return;

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->wireless_enable)
		smblib_somc_handle_wireless_exclusion(chg);

#endif

	apsd_result = smblib_update_usb_type(chg);

	if (!chg->typec_legacy_valid)
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
		smblib_somc_force_legacy_icl(chg, apsd_result->bit);
#else
		smblib_force_legacy_icl(chg, apsd_result->pst);
#endif

	switch (apsd_result->bit) {
	case SDP_CHARGER_BIT:
	case CDP_CHARGER_BIT:
		/* if not DCP, Enable pd here */
		vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
				false, 0);
		if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB
						|| chg->use_extcon)
			smblib_notify_device_mode(chg, true);
		break;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	case FLOAT_CHARGER_BIT:
		if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER)
			determine_charger_type(chg);
		/* fall through */
	case OCP_CHARGER_BIT:
#else
	case OCP_CHARGER_BIT:
	case FLOAT_CHARGER_BIT:
#endif
		/* if not DCP then no hvdcp timeout happens, Enable pd here. */
		vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
				false, 0);
		break;
	case DCP_CHARGER_BIT:
		if (chg->wa_flags & QC_CHARGER_DETECTION_WA_BIT)
			schedule_delayed_work(&chg->hvdcp_detect_work,
					      msecs_to_jiffies(HVDCP_DET_MS));
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
		if (chg->somc_hvdcp_disable_by_dt)
			smblib_handle_hvdcp_check_timeout(chg, true, false);
#endif
		break;
	default:
		break;
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: apsd-done rising; %s detected\n",
		   apsd_result->name);
}

irqreturn_t smblib_handle_usb_source_change(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc = 0;
	u8 stat;

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (chg->typec_en_dis_active) {
		smblib_dbg(chg, PR_SOMC,
			"ignored handler during typec_en_dis_active\n");
		return IRQ_HANDLED;
	}
#else
	if (chg->fake_usb_insertion)
		return IRQ_HANDLED;
#endif

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", stat);

	if ((chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
			&& (stat & APSD_DTC_STATUS_DONE_BIT)
			&& !chg->uusb_apsd_rerun_done) {
		/*
		 * Force re-run APSD to handle slow insertion related
		 * charger-mis-detection.
		 */
		chg->uusb_apsd_rerun_done = true;
		smblib_rerun_apsd(chg);
		return IRQ_HANDLED;
	}

	smblib_handle_apsd_done(chg,
		(bool)(stat & APSD_DTC_STATUS_DONE_BIT));

	smblib_handle_hvdcp_detect_done(chg,
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_check_timeout(chg,
		(bool)(stat & HVDCP_CHECK_TIMEOUT_BIT),
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_3p0_auth_done(chg,
		(bool)(stat & QC_AUTH_DONE_STATUS_BIT));

	smblib_handle_sdp_enumeration_done(chg,
		(bool)(stat & ENUMERATION_DONE_BIT));

	smblib_handle_slow_plugin_timeout(chg,
		(bool)(stat & SLOW_PLUGIN_TIMEOUT_BIT));

	smblib_hvdcp_adaptive_voltage_change(chg);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_somc_thermal_icl_change(chg);
#endif

	power_supply_changed(chg->usb_psy);

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", stat);

	return IRQ_HANDLED;
}

static int typec_try_sink(struct smb_charger *chg)
{
	union power_supply_propval val;
	bool debounce_done, vbus_detected, sink;
	u8 stat;
	int exit_mode = ATTACHED_SRC, rc;
	int typec_mode;

	if (!(*chg->try_sink_enabled))
		return ATTACHED_SRC;

	typec_mode = smblib_get_prop_typec_mode(chg);
	if (typec_mode == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER
		|| typec_mode == POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY)
		return ATTACHED_SRC;

	/*
	 * Try.SNK entry status - ATTACHWAIT.SRC state and detected Rd-open
	 * or RD-Ra for TccDebounce time.
	 */

	/* ignore typec interrupt while try.snk WIP */
	chg->try_sink_active = true;

	/* force SNK mode */
	val.intval = POWER_SUPPLY_TYPEC_PR_SINK;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set UFP mode rc=%d\n", rc);
		goto try_sink_exit;
	}

	/* reduce Tccdebounce time to ~20ms */
	rc = smblib_masked_write(chg, MISC_CFG_REG,
			TCC_DEBOUNCE_20MS_BIT, TCC_DEBOUNCE_20MS_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set MISC_CFG_REG rc=%d\n", rc);
		goto try_sink_exit;
	}

	/*
	 * give opportunity to the other side to be a SRC,
	 * for tDRPTRY + Tccdebounce time
	 */
	msleep(120);

	rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n",
				rc);
		goto try_sink_exit;
	}

	debounce_done = stat & TYPEC_DEBOUNCE_DONE_STATUS_BIT;

	if (!debounce_done)
		/*
		 * The other side didn't switch to source, either it
		 * is an adamant sink or is removed go back to showing Rp
		 */
		goto try_wait_src;

	/*
	 * We are in force sink mode and the other side has switched to
	 * showing Rp. Config DRP in case the other side removes Rp so we
	 * can quickly (20ms) switch to showing our Rp. Note that the spec
	 * needs us to show Rp for 80mS while the drp DFP residency is just
	 * 54mS. But 54mS is plenty time for us to react and force Rp for
	 * the remaining 26mS.
	 */
	val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set DFP mode rc=%d\n",
				rc);
		goto try_sink_exit;
	}

	/*
	 * while other side is Rp, wait for VBUS from it; exit if other side
	 * removes Rp
	 */
	do {
		rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n",
					rc);
			goto try_sink_exit;
		}

		debounce_done = stat & TYPEC_DEBOUNCE_DONE_STATUS_BIT;
		vbus_detected = stat & TYPEC_VBUS_STATUS_BIT;

		/* Successfully transitioned to ATTACHED.SNK */
		if (vbus_detected && debounce_done) {
			exit_mode = ATTACHED_SINK;
			goto try_sink_exit;
		}

		/*
		 * Ensure sink since drp may put us in source if other
		 * side switches back to Rd
		 */
		sink = !(stat &  UFP_DFP_MODE_STATUS_BIT);

		usleep_range(1000, 2000);
	} while (debounce_done && sink);

try_wait_src:
	/*
	 * Transition to trywait.SRC state. check if other side still wants
	 * to be SNK or has been removed.
	 */
	val.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set UFP mode rc=%d\n", rc);
		goto try_sink_exit;
	}

	/* Need to be in this state for tDRPTRY time, 75ms~150ms */
	msleep(80);

	rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		goto try_sink_exit;
	}

	debounce_done = stat & TYPEC_DEBOUNCE_DONE_STATUS_BIT;

	if (debounce_done)
		/* the other side wants to be a sink */
		exit_mode = ATTACHED_SRC;
	else
		/* the other side is detached */
		exit_mode = UNATTACHED_SINK;

try_sink_exit:
	/* release forcing of SRC/SNK mode */
	val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0)
		smblib_err(chg, "Couldn't set DFP mode rc=%d\n", rc);

	/* revert Tccdebounce time back to ~120ms */
	rc = smblib_masked_write(chg, MISC_CFG_REG, TCC_DEBOUNCE_20MS_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set MISC_CFG_REG rc=%d\n", rc);

	chg->try_sink_active = false;

	return exit_mode;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
static void smblib_somc_typec_cur_source(struct smb_charger *chg, bool en180UA)
{
	int rc;
	/* change CUR_SOURCE to advertise current */
	rc = smblib_masked_write(chg, TYPE_C_CFG_2_REG,
				EN_80UA_180UA_CUR_SOURCE_BIT,
				en180UA ? EN_80UA_180UA_CUR_SOURCE_BIT: 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't change cur source rc=%d\n", rc);
}
#endif

static void typec_sink_insertion(struct smb_charger *chg)
{
	int exit_mode;
	int typec_mode;

	exit_mode = typec_try_sink(chg);

	if (exit_mode != ATTACHED_SRC) {
		smblib_usb_typec_change(chg);
		return;
	}

	typec_mode = smblib_get_prop_typec_mode(chg);
	if (typec_mode == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER)
		chg->is_audio_adapter = true;

	/* when a sink is inserted we should not wait on hvdcp timeout to
	 * enable pd
	 */
	vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
			false, 0);
	if (chg->use_extcon) {
		smblib_notify_usb_host(chg, true);
		chg->otg_present = true;
	}
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_somc_typec_cur_source(chg, false);
#endif
}

static void typec_sink_removal(struct smb_charger *chg)
{
	smblib_set_charge_param(chg, &chg->param.freq_boost,
			chg->chg_freq.freq_above_otg_threshold);
	chg->boost_current_ua = 0;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_somc_typec_cur_source(chg, true);
#endif
}

static void smblib_handle_typec_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;
	union power_supply_propval val;

	chg->cc2_detach_wa_active = false;

	rc = smblib_request_dpdm(chg, false);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}

	/* reset APSD voters */
	vote(chg->apsd_disable_votable, PD_HARD_RESET_VOTER, false, 0);
	vote(chg->apsd_disable_votable, PD_VOTER, false, 0);

	cancel_delayed_work_sync(&chg->pl_enable_work);
	cancel_delayed_work_sync(&chg->hvdcp_detect_work);

	/* reset input current limit voters */
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 0);
	vote(chg->usb_icl_votable, HIGH_VOLTAGE_VOTER, false, 0);
#else
	vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 100000);
#endif
	vote(chg->usb_icl_votable, PD_VOTER, false, 0);
	vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	vote(chg->usb_icl_votable, PL_USBIN_USBIN_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, OTG_VOTER, false, 0);
	vote(chg->usb_icl_votable, CTM_VOTER, false, 0);
	vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);

	/* reset hvdcp voters */
	vote(chg->hvdcp_disable_votable_indirect, VBUS_CC_SHORT_VOTER, true, 0);
	vote(chg->hvdcp_disable_votable_indirect, PD_INACTIVE_VOTER, true, 0);
	vote(chg->hvdcp_hw_inov_dis_votable, OV_VOTER, false, 0);

	/* reset power delivery voters */
	vote(chg->pd_allowed_votable, PD_VOTER, false, 0);
	vote(chg->pd_disallowed_votable_indirect, CC_DETACHED_VOTER, true, 0);
	vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER, true, 0);

	/* reset usb irq voters */
	vote(chg->usb_irq_enable_votable, PD_VOTER, false, 0);
	vote(chg->usb_irq_enable_votable, QC_VOTER, false, 0);

	/* reset parallel voters */
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->pl_disable_votable, PL_FCC_LOW_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0);
	chg->vconn_attempts = 0;
	chg->otg_attempts = 0;
	chg->pulse_cnt = 0;
	chg->usb_icl_delta_ua = 0;
	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
	chg->pd_active = 0;
	chg->pd_hard_reset = false;
	chg->typec_legacy_valid = false;

#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
	/* write back the default FLOAT charger configuration */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				(u8)FLOAT_OPTIONS_MASK, chg->float_cfg);
	if (rc < 0)
		smblib_err(chg, "Couldn't write float charger options rc=%d\n",
			rc);
#endif

	/* reset back to 120mS tCC debounce */
	rc = smblib_masked_write(chg, MISC_CFG_REG, TCC_DEBOUNCE_20MS_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set 120mS tCC debounce rc=%d\n", rc);

	/*
	 * if non-compliant charger caused UV, restore original max pulses
	 * and turn SUSPEND_ON_COLLAPSE_USBIN_BIT back on.
	 */
	if (chg->non_compliant_chg_detected) {
		rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
				HVDCP_PULSE_COUNT_MAX_QC2_MASK,
				chg->qc2_max_pulses);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore max pulses rc=%d\n",
					rc);

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				SUSPEND_ON_COLLAPSE_USBIN_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn on SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		chg->non_compliant_chg_detected = false;
	}

	/* enable APSD CC trigger for next insertion */
	rc = smblib_masked_write(chg, TYPE_C_CFG_REG,
				APSD_START_ON_CC_BIT, APSD_START_ON_CC_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable APSD_START_ON_CC rc=%d\n", rc);

	if (chg->wa_flags & QC_AUTH_INTERRUPT_WA_BIT) {
		/* re-enable AUTH_IRQ_EN_CFG_BIT */
		rc = smblib_masked_write(chg,
				USBIN_SOURCE_CHANGE_INTRPT_ENB_REG,
				AUTH_IRQ_EN_CFG_BIT, AUTH_IRQ_EN_CFG_BIT);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't enable QC auth setting rc=%d\n", rc);
	}

	/* reconfigure allowed voltage for HVDCP */
	rc = smblib_set_adapter_allowance(chg,
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0)
		smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V rc=%d\n",
			rc);

	if (chg->is_audio_adapter)
		/* wait for the audio driver to lower its en gpio */
		msleep(*chg->audio_headset_drp_wait_ms);

	chg->is_audio_adapter = false;

	/* enable DRP */
	val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable DRP rc=%d\n", rc);

	/* HW controlled CC_OUT */
	rc = smblib_masked_write(chg, TAPER_TIMER_SEL_CFG_REG,
							TYPEC_SPARE_CFG_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable HW cc_out rc=%d\n", rc);

	/* restore crude sensor if PM660/PMI8998 */
	if (chg->wa_flags & TYPEC_PBS_WA_BIT) {
		rc = smblib_write(chg, TM_IO_DTEST4_SEL, 0xA5);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore crude sensor rc=%d\n",
				rc);
	}

	mutex_lock(&chg->vconn_oc_lock);
	if (!chg->vconn_en)
		goto unlock;

	smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 VCONN_EN_VALUE_BIT, 0);
	chg->vconn_en = false;

unlock:
	mutex_unlock(&chg->vconn_oc_lock);

	/* clear exit sink based on cc */
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
						EXIT_SNK_BASED_ON_CC_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't clear exit_sink_based_on_cc rc=%d\n",
				rc);

	typec_sink_removal(chg);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	chg->real_charger_type = POWER_SUPPLY_TYPE_UNKNOWN;
	chg->usb_params.apsd_result_bit = 0;
#else
	smblib_update_usb_type(chg);
#endif

	if (chg->use_extcon) {
		if (chg->otg_present)
			smblib_notify_usb_host(chg, false);
		else
			smblib_notify_device_mode(chg, false);
	}
	chg->otg_present = false;
}

static void smblib_handle_typec_insertion(struct smb_charger *chg)
{
	int rc;

	vote(chg->pd_disallowed_votable_indirect, CC_DETACHED_VOTER, false, 0);

	/* disable APSD CC trigger since CC is attached */
	rc = smblib_masked_write(chg, TYPE_C_CFG_REG, APSD_START_ON_CC_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable APSD_START_ON_CC rc=%d\n",
									rc);

	if (chg->typec_status[3] & UFP_DFP_MODE_STATUS_BIT) {
		typec_sink_insertion(chg);
	} else {
		rc = smblib_request_dpdm(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);
		typec_sink_removal(chg);
	}
}

static void smblib_handle_rp_change(struct smb_charger *chg, int typec_mode)
{
	int rp_ua;
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	/*
	 * In SOMC specification, ICL setting with Rp is not executed
	 * when a used cable is legacy AtoC, or a connected type-c device
	 * is not a default/medium/high current source device.
	 */
	if (chg->typec_status[4] & TYPEC_LEGACY_CABLE_STATUS_BIT)
		return;

	if ((typec_mode != POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) &&
			(typec_mode != POWER_SUPPLY_TYPEC_SOURCE_MEDIUM) &&
			(typec_mode != POWER_SUPPLY_TYPEC_SOURCE_HIGH))
		return;

	/*
	 * A connected power supply type is checked in the Qualcomm original
	 * code, but APSD result bit is used in SOMC implementation code.
	 * Therefore, this check also follows it.
	 */
	if ((apsd->bit != DCP_CHARGER_BIT)
		&& (apsd->bit != FLOAT_CHARGER_BIT))
		return;
#endif

	if ((apsd->pst != POWER_SUPPLY_TYPE_USB_DCP)
		&& (apsd->pst != POWER_SUPPLY_TYPE_USB_FLOAT))
		return;

	/*
	 * if APSD indicates FLOAT and the USB stack had detected SDP,
	 * do not respond to Rp changes as we do not confirm that its
	 * a legacy cable
	 */
	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB)
		return;
	/*
	 * We want the ICL vote @ 100mA for a FLOAT charger
	 * until the detection by the USB stack is complete.
	 * Ignore the Rp changes unless there is a
	 * pre-existing valid vote.
	 */
	if (apsd->pst == POWER_SUPPLY_TYPE_USB_FLOAT &&
		get_client_vote(chg->usb_icl_votable,
			LEGACY_UNKNOWN_VOTER) <= 100000)
		return;

	/*
	 * handle Rp change for DCP/FLOAT/OCP.
	 * Update the current only if the Rp is different from
	 * the last Rp value.
	 */
	smblib_dbg(chg, PR_MISC, "CC change old_mode=%d new_mode=%d\n",
						chg->typec_mode, typec_mode);

	rp_ua = get_rp_based_dcp_current(chg, typec_mode);
	vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, rp_ua);
}

static void smblib_handle_typec_cc_state_change(struct smb_charger *chg)
{
	int typec_mode;

	if (chg->pr_swap_in_progress)
		return;

	typec_mode = smblib_get_prop_typec_mode(chg);
	if (chg->typec_present && (typec_mode != chg->typec_mode))
		smblib_handle_rp_change(chg, typec_mode);

	chg->typec_mode = typec_mode;

	if (!chg->typec_present && chg->typec_mode != POWER_SUPPLY_TYPEC_NONE) {
		chg->typec_present = true;
		smblib_dbg(chg, PR_MISC, "TypeC %s insertion\n",
			smblib_typec_mode_name[chg->typec_mode]);
		smblib_handle_typec_insertion(chg);
	} else if (chg->typec_present &&
				chg->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
		chg->typec_present = false;
		smblib_dbg(chg, PR_MISC, "TypeC removal\n");
		smblib_handle_typec_removal(chg);
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	/* suspend usb if sink (!audio_adapter) */
	if ((chg->typec_status[3] & UFP_DFP_MODE_STATUS_BIT)
		&& chg->typec_present
		&& chg->typec_mode != POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER)
#else
	/* suspend usb if sink */
	if ((chg->typec_status[3] & UFP_DFP_MODE_STATUS_BIT)
			&& chg->typec_present)
#endif
		vote(chg->usb_icl_votable, OTG_VOTER, true, 0);
	else
		vote(chg->usb_icl_votable, OTG_VOTER, false, 0);

#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: cc-state-change; Type-C %s detected\n",
				smblib_typec_mode_name[chg->typec_mode]);
#endif
}

void smblib_usb_typec_change(struct smb_charger *chg)
{
	int rc;

	rc = smblib_multibyte_read(chg, TYPE_C_STATUS_1_REG,
							chg->typec_status, 5);
	if (rc < 0) {
		smblib_err(chg, "Couldn't cache USB Type-C status rc=%d\n", rc);
		return;
	}

	smblib_handle_typec_cc_state_change(chg);

	if (chg->typec_status[3] & TYPEC_VBUS_ERROR_STATUS_BIT)
		smblib_dbg(chg, PR_INTERRUPT, "IRQ: vbus-error\n");

	if (chg->typec_status[3] & TYPEC_VCONN_OVERCURR_STATUS_BIT)
		schedule_work(&chg->vconn_oc_work);

	power_supply_changed(chg->usb_psy);
}

irqreturn_t smblib_handle_usb_typec_change(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		cancel_delayed_work_sync(&chg->uusb_otg_work);
		vote(chg->awake_votable, OTG_DELAY_VOTER, true, 0);
		smblib_dbg(chg, PR_INTERRUPT, "Scheduling OTG work\n");
		schedule_delayed_work(&chg->uusb_otg_work,
				msecs_to_jiffies(chg->otg_delay_ms));
		return IRQ_HANDLED;
	}

	if (chg->cc2_detach_wa_active || chg->typec_en_dis_active ||
					 chg->try_sink_active) {
		smblib_dbg(chg, PR_MISC | PR_INTERRUPT, "Ignoring since %s active\n",
			chg->cc2_detach_wa_active ?
			"cc2_detach_wa" : "typec_en_dis");
		return IRQ_HANDLED;
	}

	if (chg->pr_swap_in_progress) {
		smblib_dbg(chg, PR_INTERRUPT,
				"Ignoring since pr_swap_in_progress\n");
		return IRQ_HANDLED;
	}

	mutex_lock(&chg->lock);
	smblib_usb_typec_change(chg);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: typec-change; Type-C %s detected\n",
				smblib_typec_mode_name[chg->typec_mode]);
#endif
	mutex_unlock(&chg->lock);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_dc_plugin(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	u8 stat;
	bool dc_present, usb_present;
	int rc;

	smblib_somc_ctrl_inhibit(chg, false);

	if (chg->jeita_sw_ctl_en) {
		cancel_delayed_work_sync(&chg->jeita_work);
		schedule_delayed_work(&chg->jeita_work, msecs_to_jiffies(0));
	}

	rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DCIN_INT_RT_STS rc=%d\n", rc);
		goto out;
	}
	dc_present = (bool)(stat & DCIN_PLUGIN_RT_STS_BIT);
	smblib_dbg(chg, PR_SOMC, "IRQ: %s dc_present:%d\n", irq_data->name,
								dc_present);

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		goto out;
	}
	usb_present = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	if (!dc_present && !usb_present) {
		__pm_wakeup_event(
			chg->usb_removal_wakelock.lock, REMOVAL_WAKE_PERIOD);
		schedule_delayed_work(&chg->usb_removal_work,
					msecs_to_jiffies(REMOVAL_DELAY_MS));
	}
out:
#endif

	power_supply_changed(chg->dc_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_high_duty_cycle(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	chg->is_hdc = true;
	/*
	 * Disable usb IRQs after the flag set and re-enable IRQs after
	 * the flag cleared in the delayed work queue, to avoid any IRQ
	 * storming during the delays
	 */
	if (chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		disable_irq_nosync(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);

	schedule_delayed_work(&chg->clear_hdc_work, msecs_to_jiffies(60));

	return IRQ_HANDLED;
}

static void smblib_bb_removal_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bb_removal_work.work);

	vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
	vote(chg->awake_votable, BOOST_BACK_VOTER, false, 0);
}

#define BOOST_BACK_UNVOTE_DELAY_MS		750
#define BOOST_BACK_STORM_COUNT			3
#define WEAK_CHG_STORM_COUNT			8
irqreturn_t smblib_handle_switcher_power_ok(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata = &irq_data->storm_data;
	int rc, usb_icl;
	u8 stat;

	if (!(chg->wa_flags & BOOST_BACK_WA))
		return IRQ_HANDLED;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	/* skip suspending input if its already suspended by some other voter */
	usb_icl = get_effective_result(chg->usb_icl_votable);
	if ((stat & USE_USBIN_BIT) && usb_icl >= 0 && usb_icl <= USBIN_25MA)
		return IRQ_HANDLED;

	if (stat & USE_DCIN_BIT)
		return IRQ_HANDLED;

	if (is_storming(&irq_data->storm_data)) {
		/* This could be a weak charger reduce ICL */
		if (!is_client_vote_enabled(chg->usb_icl_votable,
						WEAK_CHARGER_VOTER)) {
			smblib_err(chg,
				"Weak charger detected: voting %dmA ICL\n",
				*chg->weak_chg_icl_ua / 1000);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					true, *chg->weak_chg_icl_ua);
			/*
			 * reset storm data and set the storm threshold
			 * to 3 for reverse boost detection.
			 */
			update_storm_count(wdata, BOOST_BACK_STORM_COUNT);
		} else {
			smblib_err(chg,
				"Reverse boost detected: voting 0mA to suspend input\n");
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, true, 0);
			vote(chg->awake_votable, BOOST_BACK_VOTER, true, 0);
			/*
			 * Remove the boost-back vote after a delay, to avoid
			 * permanently suspending the input if the boost-back
			 * condition is unintentionally hit.
			 */
			schedule_delayed_work(&chg->bb_removal_work,
				msecs_to_jiffies(BOOST_BACK_UNVOTE_DELAY_MS));
		}
	}

	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_wdog_bark(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_write(chg, BARK_BITE_WDOG_PET_REG, BARK_BITE_WDOG_PET_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't pet the dog rc=%d\n", rc);

	if (chg->step_chg_enabled || chg->sw_jeita_enabled)
		power_supply_changed(chg->batt_psy);

	return IRQ_HANDLED;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
irqreturn_t smblib_handle_aicl_done(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_read(chg, AICL_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	if (stat & ICL_IMIN_BIT) {
		smblib_dbg(chg, PR_SOMC,
				"ICL_IMIN is detected, suspending usbin\n");
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 0);
	}

	return IRQ_HANDLED;
}
#endif

/**************
 * Additional USB PSY getters/setters
 * that call interrupt functions
 ***************/

int smblib_get_prop_pr_swap_in_progress(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->pr_swap_in_progress;
	return 0;
}

int smblib_set_prop_pr_swap_in_progress(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;

	chg->pr_swap_in_progress = val->intval;
	/*
	 * call the cc changed irq to handle real removals while
	 * PR_SWAP was in progress
	 */
	smblib_usb_typec_change(chg);
	rc = smblib_masked_write(chg, MISC_CFG_REG, TCC_DEBOUNCE_20MS_BIT,
			val->intval ? TCC_DEBOUNCE_20MS_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set tCC debounce rc=%d\n", rc);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (val->intval)
		smblib_somc_typec_cur_source(chg, false);
#endif
	return 0;
}

/***************
 * Work Queues *
 ***************/
static void smblib_uusb_otg_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						uusb_otg_work.work);
	int rc;
	u8 stat;
	bool otg;

	rc = smblib_read(chg, TYPE_C_STATUS_3_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_3 rc=%d\n", rc);
		goto out;
	}

	otg = !!(stat & (U_USB_GND_NOVBUS_BIT | U_USB_GND_BIT));
	extcon_set_state_sync(chg->extcon, EXTCON_USB_HOST, otg);
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_STATUS_3 = 0x%02x OTG=%d\n",
			stat, otg);
	power_supply_changed(chg->usb_psy);

out:
	vote(chg->awake_votable, OTG_DELAY_VOTER, false, 0);
}


static void smblib_hvdcp_detect_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
					       hvdcp_detect_work.work);

	vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
				false, 0);
	power_supply_changed(chg->usb_psy);
}

static void bms_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bms_update_work);

	smblib_suspend_on_debug_battery(chg);

	if (chg->batt_psy)
		power_supply_changed(chg->batt_psy);
}

static void pl_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						pl_update_work);

	smblib_stat_sw_override_cfg(chg, false);
}

static void clear_hdc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						clear_hdc_work.work);

	chg->is_hdc = false;
	if (chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		enable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
}

static void rdstd_cc2_detach_work(struct work_struct *work)
{
	int rc;
	u8 stat4, stat5;
	struct smb_charger *chg = container_of(work, struct smb_charger,
						rdstd_cc2_detach_work);

	if (!chg->cc2_detach_wa_active)
		return;

	/*
	 * WA steps -
	 * 1. Enable both UFP and DFP, wait for 10ms.
	 * 2. Disable DFP, wait for 30ms.
	 * 3. Removal detected if both TYPEC_DEBOUNCE_DONE_STATUS
	 *    and TIMER_STAGE bits are gone, otherwise repeat all by
	 *    work rescheduling.
	 * Note, work will be cancelled when USB_PLUGIN rises.
	 */

	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 UFP_EN_CMD_BIT | DFP_EN_CMD_BIT,
				 UFP_EN_CMD_BIT | DFP_EN_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write TYPE_C_CTRL_REG rc=%d\n", rc);
		return;
	}

	usleep_range(10000, 11000);

	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 UFP_EN_CMD_BIT | DFP_EN_CMD_BIT,
				 UFP_EN_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write TYPE_C_CTRL_REG rc=%d\n", rc);
		return;
	}

	usleep_range(30000, 31000);

	rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat4);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return;
	}

	rc = smblib_read(chg, TYPE_C_STATUS_5_REG, &stat5);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't read TYPE_C_STATUS_5_REG rc=%d\n", rc);
		return;
	}

	if ((stat4 & TYPEC_DEBOUNCE_DONE_STATUS_BIT)
			|| (stat5 & TIMER_STAGE_2_BIT)) {
		smblib_dbg(chg, PR_MISC, "rerunning DD=%d TS2BIT=%d\n",
				(int)(stat4 & TYPEC_DEBOUNCE_DONE_STATUS_BIT),
				(int)(stat5 & TIMER_STAGE_2_BIT));
		goto rerun;
	}

	smblib_dbg(chg, PR_MISC, "Bingo CC2 Removal detected\n");
	chg->cc2_detach_wa_active = false;
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
						EXIT_SNK_BASED_ON_CC_BIT, 0);
	smblib_reg_block_restore(chg, cc2_detach_settings);

	/*
	 * Mutex acquisition deadlock can happen while cancelling this work
	 * during pd_hard_reset from the function smblib_cc2_sink_removal_exit
	 * which is called in the same lock context that we try to acquire in
	 * this work routine.
	 * Check if this work is running during pd_hard_reset and skip holding
	 * mutex if lock is already held.
	 */
	if (!chg->in_chg_lock)
		mutex_lock(&chg->lock);
	smblib_usb_typec_change(chg);
	if (!chg->in_chg_lock)
		mutex_unlock(&chg->lock);

	return;

rerun:
	schedule_work(&chg->rdstd_cc2_detach_work);
}

static void smblib_otg_oc_exit(struct smb_charger *chg, bool success)
{
	int rc;

	chg->otg_attempts = 0;
	if (!success) {
		smblib_err(chg, "OTG soft start failed\n");
		chg->otg_en = false;
	}

	smblib_dbg(chg, PR_OTG, "enabling VBUS < 1V check\n");
	rc = smblib_masked_write(chg, OTG_CFG_REG,
					QUICKSTART_OTG_FASTROLESWAP_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable VBUS < 1V check rc=%d\n", rc);
}

#define MAX_OC_FALLING_TRIES 10
static void smblib_otg_oc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
								otg_oc_work);
	int rc, i;
	u8 stat;

	if (!chg->vbus_vreg || !chg->vbus_vreg->rdev)
		return;

	smblib_err(chg, "over-current detected on VBUS\n");
	mutex_lock(&chg->otg_oc_lock);
	if (!chg->otg_en)
		goto unlock;

	smblib_dbg(chg, PR_OTG, "disabling VBUS < 1V check\n");
	smblib_masked_write(chg, OTG_CFG_REG,
					QUICKSTART_OTG_FASTROLESWAP_BIT,
					QUICKSTART_OTG_FASTROLESWAP_BIT);

	/*
	 * If 500ms has passed and another over-current interrupt has not
	 * triggered then it is likely that the software based soft start was
	 * successful and the VBUS < 1V restriction should be re-enabled.
	 */
	schedule_delayed_work(&chg->otg_ss_done_work, msecs_to_jiffies(500));

	rc = _smblib_vbus_regulator_disable(chg->vbus_vreg->rdev);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable VBUS rc=%d\n", rc);
		goto unlock;
	}

	if (++chg->otg_attempts > OTG_MAX_ATTEMPTS) {
		cancel_delayed_work_sync(&chg->otg_ss_done_work);
		smblib_err(chg, "OTG failed to enable after %d attempts\n",
			   chg->otg_attempts - 1);
		smblib_otg_oc_exit(chg, false);
		goto unlock;
	}

	/*
	 * The real time status should go low within 10ms. Poll every 1-2ms to
	 * minimize the delay when re-enabling OTG.
	 */
	for (i = 0; i < MAX_OC_FALLING_TRIES; ++i) {
		usleep_range(1000, 2000);
		rc = smblib_read(chg, OTG_BASE + INT_RT_STS_OFFSET, &stat);
		if (rc >= 0 && !(stat & OTG_OVERCURRENT_RT_STS_BIT))
			break;
	}

	if (i >= MAX_OC_FALLING_TRIES) {
		cancel_delayed_work_sync(&chg->otg_ss_done_work);
		smblib_err(chg, "OTG OC did not fall after %dms\n",
						2 * MAX_OC_FALLING_TRIES);
		smblib_otg_oc_exit(chg, false);
		goto unlock;
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	somc_usb_otg_regulator_ocp_notify(chg);
#else
	smblib_dbg(chg, PR_OTG, "OTG OC fell after %dms\n", 2 * i + 1);
	rc = _smblib_vbus_regulator_enable(chg->vbus_vreg->rdev);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable VBUS rc=%d\n", rc);
		goto unlock;
	}
#endif

unlock:
	mutex_unlock(&chg->otg_oc_lock);
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && !defined(CONFIG_ARCH_SONY_TAMA)
static void somc_ocp_otg_wa_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
								ocp_otg_wa_work);

	smblib_err(chg, "over-current detected on VBUS\n");
	somc_usb_otg_regulator_ocp_notify(chg);
}
#endif

static void smblib_vconn_oc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
								vconn_oc_work);
	int rc, i;
	u8 stat;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		return;

	smblib_err(chg, "over-current detected on VCONN\n");
	if (!chg->vconn_vreg || !chg->vconn_vreg->rdev)
		return;

	mutex_lock(&chg->vconn_oc_lock);
	rc = _smblib_vconn_regulator_disable(chg->vconn_vreg->rdev);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable VCONN rc=%d\n", rc);
		goto unlock;
	}

	if (++chg->vconn_attempts > VCONN_MAX_ATTEMPTS) {
		smblib_err(chg, "VCONN failed to enable after %d attempts\n",
			   chg->vconn_attempts - 1);
		chg->vconn_en = false;
		chg->vconn_attempts = 0;
		goto unlock;
	}

	/*
	 * The real time status should go low within 10ms. Poll every 1-2ms to
	 * minimize the delay when re-enabling OTG.
	 */
	for (i = 0; i < MAX_OC_FALLING_TRIES; ++i) {
		usleep_range(1000, 2000);
		rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
		if (rc >= 0 && !(stat & TYPEC_VCONN_OVERCURR_STATUS_BIT))
			break;
	}

	if (i >= MAX_OC_FALLING_TRIES) {
		smblib_err(chg, "VCONN OC did not fall after %dms\n",
						2 * MAX_OC_FALLING_TRIES);
		chg->vconn_en = false;
		chg->vconn_attempts = 0;
		goto unlock;
	}
	smblib_dbg(chg, PR_OTG, "VCONN OC fell after %dms\n", 2 * i + 1);

	rc = _smblib_vconn_regulator_enable(chg->vconn_vreg->rdev);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable VCONN rc=%d\n", rc);
		goto unlock;
	}

unlock:
	mutex_unlock(&chg->vconn_oc_lock);
}

static void smblib_otg_ss_done_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							otg_ss_done_work.work);
	int rc;
	bool success = false;
	u8 stat;

	mutex_lock(&chg->otg_oc_lock);
	rc = smblib_read(chg, OTG_STATUS_REG, &stat);
	if (rc < 0)
		smblib_err(chg, "Couldn't read OTG status rc=%d\n", rc);
	else if (stat & BOOST_SOFTSTART_DONE_BIT)
		success = true;

	smblib_otg_oc_exit(chg, success);
	mutex_unlock(&chg->otg_oc_lock);
}

static void smblib_icl_change_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							icl_change_work.work);
	int rc, settled_ua;

	rc = smblib_get_charge_param(chg, &chg->param.icl_stat, &settled_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
		return;
	}

	power_supply_changed(chg->usb_main_psy);

	smblib_dbg(chg, PR_INTERRUPT, "icl_settled=%d\n", settled_ua);
}

static void smblib_pl_enable_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							pl_enable_work.work);

	smblib_dbg(chg, PR_PARALLEL, "timer expired, enabling parallel\n");
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);
}

static void smblib_legacy_detection_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							legacy_detection_work);
	int rc;
	u8 stat;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	bool legacy, rp_default;
	union power_supply_propval val;
#else
	bool legacy, rp_high;
#endif

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	mutex_lock(&chg->legacy_detection_lock);
	rc = smblib_get_prop_batt_status(chg, &val);
	if (rc < 0)
		val.intval = POWER_SUPPLY_STATUS_CHARGING;
	chg->status_before_typec_en_dis_active = val.intval;
#else
	mutex_lock(&chg->lock);
#endif
	chg->typec_en_dis_active = true;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	smblib_dbg(chg, PR_SOMC, "running legacy unknown workaround\n");
#else
	smblib_dbg(chg, PR_MISC, "running legacy unknown workaround\n");
#endif
	rc = smblib_masked_write(chg,
				TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				TYPEC_DISABLE_CMD_BIT,
				TYPEC_DISABLE_CMD_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable type-c rc=%d\n", rc);

	/* wait for the adapter to turn off VBUS */
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	msleep(400);
#else
	msleep(1000);
#endif

	smblib_dbg(chg, PR_MISC, "legacy workaround enabling typec\n");

	rc = smblib_masked_write(chg,
				TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				TYPEC_DISABLE_CMD_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable type-c rc=%d\n", rc);

	/* wait for type-c detection to complete */
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	msleep(200);
#else
	msleep(400);
#endif

	rc = smblib_read(chg, TYPE_C_STATUS_5_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read typec stat5 rc = %d\n", rc);
		goto unlock;
	}

	chg->typec_legacy_valid = true;
#if !defined(CONFIG_SOMC_CHARGER_EXTENSION)
	vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, false, 0);
#endif
	legacy = stat & TYPEC_LEGACY_CABLE_STATUS_BIT;
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0)
		val.intval = 0;
	rp_default = chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
	if (val.intval) {
		if (!legacy || rp_default)
			vote(chg->hvdcp_disable_votable_indirect,
					VBUS_CC_SHORT_VOTER, false, 0);
		if (get_effective_result(chg->hvdcp_enable_votable) == 0)
			determine_charger_type(chg);
	}
#else
	rp_high = chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH;
	smblib_dbg(chg, PR_MISC, "legacy workaround done legacy = %d rp_high = %d\n",
			legacy, rp_high);
	if (!legacy || !rp_high)
		vote(chg->hvdcp_disable_votable_indirect, VBUS_CC_SHORT_VOTER,
								false, 0);
#endif

unlock:
	chg->typec_en_dis_active = false;
	smblib_usb_typec_change(chg);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	mutex_unlock(&chg->legacy_detection_lock);
#else
	mutex_unlock(&chg->lock);
#endif
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
static void smblib_somc_fake_charging_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							fake_charging_work);
	chg->duration_fake_charging = true;
	msleep(500);
	chg->duration_fake_charging = false;
}

#if defined(CONFIG_ARCH_SONY_TAMA)
static void smblib_somc_wireless_v_chg_work(struct work_struct *work)
{
	int rc;
	int wireless_lim_v;
	union power_supply_propval pval = {0, };
	bool dc_online;
	struct smb_charger *chg = container_of(work, struct smb_charger,
						wireless_v_chg_work.work);
	int lv = chg->system_temp_level;

	if (!chg->wireless_enable)
		return;

	if (!chg->wireless_psy)
		chg->wireless_psy = power_supply_get_by_name("wireless");

	if (!chg->wireless_psy) {
		/* retry worker*/
		smblib_dbg(chg, PR_SOMC,
			   "Reschedule v_chg_work due to wireless_psy error\n");
		schedule_delayed_work(&chg->wireless_v_chg_work,
					msecs_to_jiffies(500));
		return;
	}

	if (IS_ERR_OR_NULL(chg->thermal_wireless_v_limit) ||
		lv > chg->thermal_wireless_v_limit_levels - 1) {
		smblib_err(chg, "Invalid parameter of wireless_v_limit\n");
		return;
	}
	wireless_lim_v = chg->thermal_wireless_v_limit[lv];

	rc = smblib_get_prop_dc_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get dc online property rc=%d\n",
			rc);
		return;
	}
	dc_online = (bool)pval.intval;

	smblib_dbg(chg, PR_MISC,
			"wireless_lim_v: %d (from %d) dc_online: %d\n",
			wireless_lim_v, chg->wireless_thermal_limit_voltage,
			dc_online);
	if (wireless_lim_v != chg->wireless_thermal_limit_voltage) {
		rc = smblib_somc_report_wireless_thermal_limit(chg,
							wireless_lim_v);
		if (rc < 0)
			smblib_err(chg, "Couldn't report therm limit rc=%d\n",
									rc);

		chg->wireless_thermal_limit_voltage = wireless_lim_v;
	}
}

#define FV_JEITA_WARM_RB_WA_EXIT_THRESH_UV	200000
#define JEITA_WORK_DELAY_RETRY_MS		500
#define JEITA_WORK_DELAY_CHARGING_MS		5000
#define JEITA_WORK_DELAY_DISCHARGING_MS		30000
#define JEITA_FAKE_CARGING_TIME_MS		500
static void smblib_somc_jeita_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							jeita_work.work);
	union power_supply_propval pval = {0, };
	int rc;
	int batt_temp = 0;
	int skin_temp = 0;
	int wlc_temp = 0;
	u8 reg;
	u8 chg_stat;
	bool usbin_valid = false;
	bool dcin_valid = false;
	bool skin_temp_failed = false;
	bool wlc_temp_failed = false;
	int interval_ms;
	int synth_cond;
	int vbatt;
	int batt_condition;
	int skin_condition;
	int wlc_condition;
	bool condition_changed = false;

	if (!chg->jeita_sw_ctl_en)
		return;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &reg);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}
	usbin_valid = (bool)(reg & USBIN_PLUGIN_RT_STS_BIT);

	if (chg->jeita_use_wlc) {
		rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &reg);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't read DC_INT_RT_STS rc=%d\n",
									rc);
			return;
		}
		dcin_valid = (bool)(reg & DCIN_PLUGIN_RT_STS_BIT);
	}

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_TEMP, &pval);
	if (rc < 0)
		smblib_err(chg, "Couldn't get batt temp rc=%d\n", rc);

	batt_temp = pval.intval;

	rc = smblib_get_prop_from_bms(chg,
                               POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read VBATT rc=%d\n", rc);
		goto reschedule;
	}
	vbatt = pval.intval;

	if (chg->jeita_use_aux) {
		rc = smblib_get_prop_skin_temp(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get skin temp rc=%d\n", rc);
			skin_temp_failed = true;
		}
		skin_temp = pval.intval;

		smblib_dbg(chg, PR_MISC,
				"usbin_valid=%d batt_temp=%d sikn_temp=%d\n",
				(int)usbin_valid, batt_temp,
				skin_temp_failed ? 0 : skin_temp);
	}

	if (!chg->jeita_use_aux) {
		chg->jeita_skin_condition = TEMP_CONDITION_DEFAULT;
	} else if (!skin_temp_failed) {
		skin_condition = chg->jeita_skin_condition;
		if (skin_temp > chg->jeita_aux_thresh_hot)
			chg->jeita_skin_condition = TEMP_CONDITION_HOT;
		else if (skin_temp > chg->jeita_aux_thresh_warm)
			chg->jeita_skin_condition = TEMP_CONDITION_WARM;
		else
			chg->jeita_skin_condition = TEMP_CONDITION_NORMAL;

		if (skin_condition != chg->jeita_skin_condition)
			condition_changed = true;
	}

	/* read wireless charger temp */
	if (chg->jeita_use_wlc) {
		if (!chg->wireless_psy)
			chg->wireless_psy =
					power_supply_get_by_name("wireless");

		if (chg->wireless_psy) {
			rc = power_supply_get_property(chg->wireless_psy,
							POWER_SUPPLY_PROP_TEMP,
							&pval);
			if (rc) {
				smblib_dbg(chg, PR_MISC,
					   "Couldn't get wlc_temp rc = %d\n",
					   rc);
				wlc_temp_failed = true;
			} else {
				wlc_temp = pval.intval;
			}
			smblib_dbg(chg, PR_MISC,
					"batt_temp=%d wlc_temp=%d\n",
					batt_temp,
					wlc_temp_failed ? 0 : wlc_temp);
		} else {
			smblib_err(chg, "Couldn't get wireless_psy\n");
			wlc_temp_failed = true;
		}
	}

	if (!chg->jeita_use_wlc || wlc_temp_failed) {
		chg->jeita_wlc_condition = TEMP_CONDITION_DEFAULT;
	} else {
		wlc_condition = chg->jeita_wlc_condition;
		if (wlc_temp > chg->jeita_wlc_thresh_hot)
			chg->jeita_wlc_condition = TEMP_CONDITION_HOT;
		else if (wlc_temp > chg->jeita_wlc_thresh_warm)
			chg->jeita_wlc_condition = TEMP_CONDITION_WARM;
		else
			chg->jeita_wlc_condition = TEMP_CONDITION_NORMAL;

		if (wlc_condition != chg->jeita_wlc_condition)
			condition_changed = true;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &chg_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read charger status 1 rc=%d\n", rc);
		return;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &reg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read charger status 2 rc=%d\n", rc);
		return;
	}
	batt_condition = chg->jeita_batt_condition;
	if (reg & BAT_TEMP_STATUS_TOO_COLD_BIT)
		chg->jeita_batt_condition = TEMP_CONDITION_COLD;
	else if (reg & BAT_TEMP_STATUS_TOO_HOT_BIT)
		chg->jeita_batt_condition = TEMP_CONDITION_HOT;
	else if (reg & BAT_TEMP_STATUS_COLD_SOFT_LIMIT_BIT)
		chg->jeita_batt_condition = TEMP_CONDITION_COOL;
	else if (reg & BAT_TEMP_STATUS_HOT_SOFT_LIMIT_BIT)
		chg->jeita_batt_condition = TEMP_CONDITION_WARM;
	else
		chg->jeita_batt_condition = TEMP_CONDITION_NORMAL;

	if (batt_condition != chg->jeita_batt_condition)
		condition_changed = true;

	switch (chg->jeita_batt_condition) {
	case TEMP_CONDITION_HOT:
		synth_cond = TEMP_CONDITION_HOT;
		break;
	case TEMP_CONDITION_WARM:
		if (chg->jeita_skin_condition == TEMP_CONDITION_HOT ||
		    chg->jeita_wlc_condition == TEMP_CONDITION_HOT)
			synth_cond = TEMP_CONDITION_HOT;
		else
			synth_cond = TEMP_CONDITION_WARM;
		break;
	case TEMP_CONDITION_NORMAL:
		if (chg->jeita_skin_condition == TEMP_CONDITION_HOT ||
		    chg->jeita_wlc_condition == TEMP_CONDITION_HOT)
			synth_cond = TEMP_CONDITION_HOT;
		else if (chg->jeita_skin_condition == TEMP_CONDITION_WARM ||
			 chg->jeita_wlc_condition == TEMP_CONDITION_WARM)
			synth_cond = TEMP_CONDITION_WARM;
		else
			synth_cond = TEMP_CONDITION_NORMAL;
		break;
	case TEMP_CONDITION_COOL:
		synth_cond = TEMP_CONDITION_COOL;
		break;
	case TEMP_CONDITION_COLD:
		synth_cond = TEMP_CONDITION_COLD;
		break;
	default:
		synth_cond = TEMP_CONDITION_NORMAL;
		break;
	}
	smblib_dbg(chg, PR_MISC, "batt=%d skin=%d wlc=%d result=%d\n",
			chg->jeita_batt_condition, chg->jeita_skin_condition,
			chg->jeita_wlc_condition, synth_cond);

	if (synth_cond == TEMP_CONDITION_HOT ||
	    synth_cond == TEMP_CONDITION_COLD)
		vote(chg->chg_disable_votable, SOMC_JEITA_VOTER, true, 0);
	else
		vote(chg->chg_disable_votable, SOMC_JEITA_VOTER, false, 0);

	if (synth_cond == TEMP_CONDITION_WARM)
		vote(chg->fv_votable, SOMC_JEITA_VOTER, true,
					chg->jeita_warm_fv_uv);
	else
		vote(chg->fv_votable, SOMC_JEITA_VOTER, false, 0);

	if (synth_cond == TEMP_CONDITION_WARM && chg->jeita_warm_fcc_ua > 0)
		vote(chg->fcc_votable, SOMC_JEITA_VOTER, true,
					chg->jeita_warm_fcc_ua);
	else if (synth_cond == TEMP_CONDITION_COOL &&
						chg->jeita_cool_fcc_ua > 0)
		vote(chg->fcc_votable, SOMC_JEITA_VOTER, true,
					chg->jeita_cool_fcc_ua);
	else
		vote(chg->fcc_votable, SOMC_JEITA_VOTER, false, 0);

	/* WA for Reverse Boost */
	if (!chg->jeita_rb_warm_hi_vbatt_en &&
		usbin_valid && synth_cond == TEMP_CONDITION_WARM &&
		vbatt > chg->jeita_warm_fv_uv) {
		smblib_dbg(chg, PR_SOMC,
				"WA for RB after Warm. vbatt=%d\n",
				vbatt);
		chg->jeita_rb_warm_hi_vbatt_en = true;
		vote(chg->usb_icl_votable, SOMC_JEITA_VOTER, true, 0);
	} else if (chg->jeita_rb_warm_hi_vbatt_en &&
			(!usbin_valid || synth_cond != TEMP_CONDITION_WARM ||
			vbatt < chg->jeita_warm_fv_uv
					- FV_JEITA_WARM_RB_WA_EXIT_THRESH_UV)) {
		smblib_dbg(chg, PR_SOMC,
				"Release WA for RB after Warm. vbatt=%d\n",
				vbatt);
		vote(chg->usb_icl_votable, SOMC_JEITA_VOTER, false, 0);
		chg->jeita_rb_warm_hi_vbatt_en = false;
	}

	/* WA for holding Charge Termination after normal */
	if (usbin_valid &&
	    chg->jeita_synth_condition == TEMP_CONDITION_WARM &&
	    (synth_cond == TEMP_CONDITION_NORMAL ||
	    synth_cond == TEMP_CONDITION_COOL) &&
	    !get_effective_result(chg->chg_disable_votable) &&
	    ((chg_stat & BATTERY_CHARGER_STATUS_MASK) == TERMINATE_CHARGE ||
	    (chg_stat & BATTERY_CHARGER_STATUS_MASK) == INHIBIT_CHARGE ||
	    (chg_stat & CC_SOFT_TERMINATE_BIT) == CC_SOFT_TERMINATE_BIT)) {
		smblib_dbg(chg, PR_SOMC, "Execute WA for holding FULL\n");
		chg->jeita_keep_fake_charging = true;
		vote(chg->chg_disable_votable, SOMC_JEITA_VOTER, true, 0);
		vote(chg->chg_disable_votable, SOMC_JEITA_VOTER, false, 0);
		msleep(JEITA_FAKE_CARGING_TIME_MS);

		smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &reg);
		smblib_dbg(chg, PR_SOMC, "waiting done chg_sts1=0x%02x\n", reg);
		chg->jeita_keep_fake_charging = false;
	}

	if (synth_cond != chg->jeita_synth_condition || condition_changed ||
						chg->jeita_debug_log_interval) {
		power_supply_changed(chg->batt_psy);
		if (chg->jeita_use_aux && chg->jeita_use_wlc)
			smblib_dbg(chg, PR_SOMC,
					"JEITA: batt_temp=%d(%d) skin_temp=%d(%d) wlc=%d(%d) result:%d\n",
					batt_temp, chg->jeita_batt_condition,
					skin_temp, chg->jeita_skin_condition,
					wlc_temp, chg->jeita_wlc_condition,
					synth_cond);
		else if (chg->jeita_use_aux)
			smblib_dbg(chg, PR_SOMC,
					"JEITA: batt_temp=%d(%d) skin_temp=%d(%d) result:%d\n",
					batt_temp, chg->jeita_batt_condition,
					skin_temp, chg->jeita_skin_condition,
					synth_cond);
		else if (chg->jeita_use_wlc)
			smblib_dbg(chg, PR_SOMC,
					"JEITA: batt_temp=%d(%d) wlc=%d(%d) result:%d\n",
					batt_temp, chg->jeita_batt_condition,
					wlc_temp, chg->jeita_wlc_condition,
					synth_cond);
		else
			smblib_dbg(chg, PR_SOMC,
					"JEITA: batt_temp=%d(%d) result:%d\n",
					batt_temp, chg->jeita_batt_condition,
					synth_cond);
	}
	chg->jeita_synth_condition = synth_cond;

reschedule:
	if (chg->jeita_debug_log_interval)
		interval_ms = chg->jeita_debug_log_interval;
	else if (usbin_valid && skin_temp_failed)
		interval_ms = JEITA_WORK_DELAY_RETRY_MS;
	else if (usbin_valid && !skin_temp_failed)
		interval_ms = JEITA_WORK_DELAY_CHARGING_MS;
	else if (dcin_valid)
		interval_ms = JEITA_WORK_DELAY_CHARGING_MS;
	else
		interval_ms = JEITA_WORK_DELAY_DISCHARGING_MS;

	smblib_dbg(chg, PR_MISC, "will schedule delayed worker (%d ms)\n",
								interval_ms);

	if (chg->jeita_sw_ctl_en)
		schedule_delayed_work(&chg->jeita_work,
						msecs_to_jiffies(interval_ms));
}
#endif

static void smblib_somc_smart_charge_wdog_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
					smart_charge_wdog_work.work);

	smblib_dbg(chg, PR_SOMC, "Smart Charge Watchdog timer has expired.\n");

	mutex_lock(&chg->smart_charge_lock);
	vote(chg->chg_disable_votable, BATTCHG_SMART_EN_VOTER, false, 0);
	chg->smart_charge_suspended = false;
	mutex_unlock(&chg->smart_charge_lock);

	power_supply_changed(chg->batt_psy);
}

static void smblib_somc_removal_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
					usb_removal_work.work);

	if (chg->usb_removal_input && !chg->low_batt_shutdown_enabled) {
		/* key event for power off charge */
		smblib_dbg(chg, PR_SOMC, "input_report_key KEY_F24\n");
		input_report_key(chg->usb_removal_input, KEY_F24, 1);
		input_sync(chg->usb_removal_input);
		input_report_key(chg->usb_removal_input, KEY_F24, 0);
		input_sync(chg->usb_removal_input);
	}
}
#endif

static int smblib_create_votables(struct smb_charger *chg)
{
	int rc = 0;

	chg->fcc_votable = find_votable("FCC");
	if (chg->fcc_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FCC votable rc=%d\n", rc);
		return rc;
	}

	chg->fv_votable = find_votable("FV");
	if (chg->fv_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FV votable rc=%d\n", rc);
		return rc;
	}

	chg->usb_icl_votable = find_votable("USB_ICL");
	if (!chg->usb_icl_votable) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find USB_ICL votable rc=%d\n", rc);
		return rc;
	}

	chg->pl_disable_votable = find_votable("PL_DISABLE");
	if (chg->pl_disable_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find votable PL_DISABLE rc=%d\n", rc);
		return rc;
	}

	chg->pl_enable_votable_indirect = find_votable("PL_ENABLE_INDIRECT");
	if (chg->pl_enable_votable_indirect == NULL) {
		rc = -EINVAL;
		smblib_err(chg,
			"Couldn't find votable PL_ENABLE_INDIRECT rc=%d\n",
			rc);
		return rc;
	}

	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);

	chg->dc_suspend_votable = create_votable("DC_SUSPEND", VOTE_SET_ANY,
					smblib_dc_suspend_vote_callback,
					chg);
	if (IS_ERR(chg->dc_suspend_votable)) {
		rc = PTR_ERR(chg->dc_suspend_votable);
		return rc;
	}

	chg->dc_icl_votable = create_votable("DC_ICL", VOTE_MIN,
					smblib_dc_icl_vote_callback,
					chg);
	if (IS_ERR(chg->dc_icl_votable)) {
		rc = PTR_ERR(chg->dc_icl_votable);
		return rc;
	}

	chg->pd_disallowed_votable_indirect
		= create_votable("PD_DISALLOWED_INDIRECT", VOTE_SET_ANY,
			smblib_pd_disallowed_votable_indirect_callback, chg);
	if (IS_ERR(chg->pd_disallowed_votable_indirect)) {
		rc = PTR_ERR(chg->pd_disallowed_votable_indirect);
		return rc;
	}

	chg->pd_allowed_votable = create_votable("PD_ALLOWED",
					VOTE_SET_ANY, NULL, NULL);
	if (IS_ERR(chg->pd_allowed_votable)) {
		rc = PTR_ERR(chg->pd_allowed_votable);
		return rc;
	}

	chg->awake_votable = create_votable("AWAKE", VOTE_SET_ANY,
					smblib_awake_vote_callback,
					chg);
	if (IS_ERR(chg->awake_votable)) {
		rc = PTR_ERR(chg->awake_votable);
		return rc;
	}

	chg->chg_disable_votable = create_votable("CHG_DISABLE", VOTE_SET_ANY,
					smblib_chg_disable_vote_callback,
					chg);
	if (IS_ERR(chg->chg_disable_votable)) {
		rc = PTR_ERR(chg->chg_disable_votable);
		return rc;
	}


	chg->hvdcp_disable_votable_indirect = create_votable(
				"HVDCP_DISABLE_INDIRECT",
				VOTE_SET_ANY,
				smblib_hvdcp_disable_indirect_vote_callback,
				chg);
	if (IS_ERR(chg->hvdcp_disable_votable_indirect)) {
		rc = PTR_ERR(chg->hvdcp_disable_votable_indirect);
		return rc;
	}

	chg->hvdcp_enable_votable = create_votable("HVDCP_ENABLE",
					VOTE_SET_ANY,
					smblib_hvdcp_enable_vote_callback,
					chg);
	if (IS_ERR(chg->hvdcp_enable_votable)) {
		rc = PTR_ERR(chg->hvdcp_enable_votable);
		return rc;
	}

	chg->apsd_disable_votable = create_votable("APSD_DISABLE",
					VOTE_SET_ANY,
					smblib_apsd_disable_vote_callback,
					chg);
	if (IS_ERR(chg->apsd_disable_votable)) {
		rc = PTR_ERR(chg->apsd_disable_votable);
		return rc;
	}

	chg->hvdcp_hw_inov_dis_votable = create_votable("HVDCP_HW_INOV_DIS",
					VOTE_SET_ANY,
					smblib_hvdcp_hw_inov_dis_vote_callback,
					chg);
	if (IS_ERR(chg->hvdcp_hw_inov_dis_votable)) {
		rc = PTR_ERR(chg->hvdcp_hw_inov_dis_votable);
		return rc;
	}

	chg->usb_irq_enable_votable = create_votable("USB_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_usb_irq_enable_vote_callback,
					chg);
	if (IS_ERR(chg->usb_irq_enable_votable)) {
		rc = PTR_ERR(chg->usb_irq_enable_votable);
		return rc;
	}

	chg->typec_irq_disable_votable = create_votable("TYPEC_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_typec_irq_disable_vote_callback,
					chg);
	if (IS_ERR(chg->typec_irq_disable_votable)) {
		rc = PTR_ERR(chg->typec_irq_disable_votable);
		return rc;
	}

	chg->disable_power_role_switch
			= create_votable("DISABLE_POWER_ROLE_SWITCH",
				VOTE_SET_ANY,
				smblib_disable_power_role_switch_callback,
				chg);
	if (IS_ERR(chg->disable_power_role_switch)) {
		rc = PTR_ERR(chg->disable_power_role_switch);
		return rc;
	}
	vote(chg->disable_power_role_switch, DEFAULT_VOTER,
			chg->ufp_only_mode, 0);

	return rc;
}

static void smblib_destroy_votables(struct smb_charger *chg)
{
	if (chg->dc_suspend_votable)
		destroy_votable(chg->dc_suspend_votable);
	if (chg->usb_icl_votable)
		destroy_votable(chg->usb_icl_votable);
	if (chg->dc_icl_votable)
		destroy_votable(chg->dc_icl_votable);
	if (chg->pd_disallowed_votable_indirect)
		destroy_votable(chg->pd_disallowed_votable_indirect);
	if (chg->pd_allowed_votable)
		destroy_votable(chg->pd_allowed_votable);
	if (chg->awake_votable)
		destroy_votable(chg->awake_votable);
	if (chg->chg_disable_votable)
		destroy_votable(chg->chg_disable_votable);
	if (chg->apsd_disable_votable)
		destroy_votable(chg->apsd_disable_votable);
	if (chg->hvdcp_hw_inov_dis_votable)
		destroy_votable(chg->hvdcp_hw_inov_dis_votable);
	if (chg->typec_irq_disable_votable)
		destroy_votable(chg->typec_irq_disable_votable);
	if (chg->disable_power_role_switch)
		destroy_votable(chg->disable_power_role_switch);
}

static void smblib_iio_deinit(struct smb_charger *chg)
{
	if (!IS_ERR_OR_NULL(chg->iio.temp_chan))
		iio_channel_release(chg->iio.temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.temp_max_chan))
		iio_channel_release(chg->iio.temp_max_chan);
	if (!IS_ERR_OR_NULL(chg->iio.usbin_i_chan))
		iio_channel_release(chg->iio.usbin_i_chan);
	if (!IS_ERR_OR_NULL(chg->iio.usbin_v_chan))
		iio_channel_release(chg->iio.usbin_v_chan);
	if (!IS_ERR_OR_NULL(chg->iio.batt_i_chan))
		iio_channel_release(chg->iio.batt_i_chan);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
	if (!IS_ERR_OR_NULL(chg->iio.skin_temp_chan))
		iio_channel_release(chg->iio.skin_temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.dcin_i_chan))
		iio_channel_release(chg->iio.dcin_i_chan);
	if (!IS_ERR_OR_NULL(chg->iio.dcin_v_chan))
		iio_channel_release(chg->iio.dcin_v_chan);
#endif
}

int smblib_init(struct smb_charger *chg)
{
	int rc = 0;

	mutex_init(&chg->lock);
	mutex_init(&chg->write_lock);
	mutex_init(&chg->otg_oc_lock);
	mutex_init(&chg->vconn_oc_lock);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	mutex_init(&chg->smart_charge_lock);
	mutex_init(&chg->thermal_lock);
	mutex_init(&chg->legacy_detection_lock);
	chg->usb_removal_wakelock.lock = wakeup_source_register(chg->dev, "unplug_wakelock");
	chg->usb_removal_wakelock.enabled = true;
#endif
	INIT_WORK(&chg->bms_update_work, bms_update_work);
	INIT_WORK(&chg->pl_update_work, pl_update_work);
	INIT_WORK(&chg->rdstd_cc2_detach_work, rdstd_cc2_detach_work);
	INIT_DELAYED_WORK(&chg->hvdcp_detect_work, smblib_hvdcp_detect_work);
	INIT_DELAYED_WORK(&chg->clear_hdc_work, clear_hdc_work);
	INIT_WORK(&chg->otg_oc_work, smblib_otg_oc_work);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && !defined(CONFIG_ARCH_SONY_TAMA)
	INIT_WORK(&chg->ocp_otg_wa_work, somc_ocp_otg_wa_work);
#endif
	INIT_WORK(&chg->vconn_oc_work, smblib_vconn_oc_work);
	INIT_DELAYED_WORK(&chg->otg_ss_done_work, smblib_otg_ss_done_work);
	INIT_DELAYED_WORK(&chg->icl_change_work, smblib_icl_change_work);
	INIT_DELAYED_WORK(&chg->pl_enable_work, smblib_pl_enable_work);
	INIT_WORK(&chg->legacy_detection_work, smblib_legacy_detection_work);
	INIT_DELAYED_WORK(&chg->uusb_otg_work, smblib_uusb_otg_work);
	INIT_DELAYED_WORK(&chg->bb_removal_work, smblib_bb_removal_work);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	INIT_WORK(&chg->fake_charging_work, smblib_somc_fake_charging_work);
	INIT_WORK(&chg->thermal_fake_charging_work,
				smblib_somc_thermal_fake_charging_work);
#if defined(CONFIG_ARCH_SONY_TAMA)
	INIT_WORK(&chg->wireless_wa_fake_charging_work,
					smblib_somc_wlc_fake_charging_work);
	INIT_DELAYED_WORK(&chg->jeita_work, smblib_somc_jeita_work);
	INIT_DELAYED_WORK(&chg->wireless_v_chg_work,
					smblib_somc_wireless_v_chg_work);
#endif
	INIT_DELAYED_WORK(&chg->smart_charge_wdog_work,
					smblib_somc_smart_charge_wdog_work);
	INIT_DELAYED_WORK(&chg->usb_removal_work, smblib_somc_removal_work);

	/* register input device */
	chg->usb_removal_input = input_allocate_device();
	if (!chg->usb_removal_input) {
		dev_err(chg->dev,
			"can't allocate unplug virtual button\n");
		rc = -ENOMEM;
		return rc;
	}
	input_set_capability(chg->usb_removal_input, EV_KEY, KEY_F24);
	chg->usb_removal_input->name = "SOMC USB Removal";
	chg->usb_removal_input->dev.parent = chg->dev;

	rc = input_register_device(chg->usb_removal_input);
	if (rc) {
		dev_err(chg->dev,
			"can't register power key: %d\n", rc);
		rc = -ENOMEM;
		return rc;
	}
#endif
	chg->fake_capacity = -EINVAL;
	chg->fake_input_current_limited = -EINVAL;
	chg->fake_batt_status = -EINVAL;

	switch (chg->mode) {
	case PARALLEL_MASTER:
		rc = qcom_batt_init(&chg->chg_param);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_batt_init rc=%d\n",
				rc);
			return rc;
		}

		rc = qcom_step_chg_init(chg->dev, chg->step_chg_enabled,
						chg->sw_jeita_enabled, true);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_step_chg_init rc=%d\n",
				rc);
			return rc;
		}

		rc = smblib_create_votables(chg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't create votables rc=%d\n",
				rc);
			return rc;
		}

		rc = smblib_register_notifier(chg);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't register notifier rc=%d\n", rc);
			return rc;
		}

		chg->bms_psy = power_supply_get_by_name("bms");
		chg->pl.psy = power_supply_get_by_name("parallel");
		if (chg->pl.psy) {
			rc = smblib_stat_sw_override_cfg(chg, false);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't config stat sw rc=%d\n", rc);
				return rc;
			}
		}
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && defined(CONFIG_ARCH_SONY_TAMA)
		chg->wireless_psy = power_supply_get_by_name("wireless");
#endif
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

	return rc;
}

int smblib_deinit(struct smb_charger *chg)
{
	switch (chg->mode) {
	case PARALLEL_MASTER:
		cancel_work_sync(&chg->bms_update_work);
		cancel_work_sync(&chg->pl_update_work);
		cancel_work_sync(&chg->rdstd_cc2_detach_work);
		cancel_delayed_work_sync(&chg->hvdcp_detect_work);
		cancel_delayed_work_sync(&chg->clear_hdc_work);
		cancel_work_sync(&chg->otg_oc_work);
#if defined(CONFIG_SOMC_CHARGER_EXTENSION) && !defined(CONFIG_ARCH_SONY_TAMA)
		cancel_work_sync(&chg->ocp_otg_wa_work);
#endif
		cancel_work_sync(&chg->vconn_oc_work);
		cancel_delayed_work_sync(&chg->otg_ss_done_work);
		cancel_delayed_work_sync(&chg->icl_change_work);
		cancel_delayed_work_sync(&chg->pl_enable_work);
		cancel_work_sync(&chg->legacy_detection_work);
		cancel_delayed_work_sync(&chg->uusb_otg_work);
		cancel_delayed_work_sync(&chg->bb_removal_work);
		power_supply_unreg_notifier(&chg->nb);
		smblib_destroy_votables(chg);
		qcom_step_chg_deinit();
		qcom_batt_deinit();
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
	if (chg->usb_removal_input) {
		input_free_device(chg->usb_removal_input);
		chg->usb_removal_input = NULL;
	}
	if (chg->usb_removal_wakelock.enabled) {
		wakeup_source_unregister(chg->usb_removal_wakelock.lock);
		chg->usb_removal_wakelock.enabled = false;
	}
#endif

	smblib_iio_deinit(chg);

	return 0;
}

#if defined(CONFIG_SOMC_CHARGER_EXTENSION)
/**************************
 * SOMC feature functions *
 **************************/

void smblib_somc_thermal_fcc_change(struct smb_charger *chg)
{
	int lv = chg->system_temp_level;

	smblib_dbg(chg, PR_MISC, "thermal fcc change lv=%d\n", lv);

	if (IS_ERR_OR_NULL(chg->thermal_fcc_ua)) {
		smblib_dbg(chg, PR_MISC, "thermal fcc table is NULL\n");
		return;
	}

	if (lv > chg->thermal_fcc_levels - 1) {
		smblib_dbg(chg, PR_MISC, "thermal lv is out of range\n");
		return;
	}

	if (chg->thermal_fcc_ua[lv] > 0) {
		vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, true,
						chg->thermal_fcc_ua[lv]);
		vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER, false, 0);
	} else {
		vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER, true, 0);
		vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, false, 0);
	}
}

void smblib_somc_thermal_icl_change(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	int pulses;
	int icl = 0;
	int lv = chg->system_temp_level;
	int type;

	mutex_lock(&chg->thermal_lock);
	smblib_dbg(chg, PR_MISC, "thermal icl change lv=%d\n", lv);

	if (IS_ERR_OR_NULL(chg->thermal_lo_volt_icl_ua) ||
	    IS_ERR_OR_NULL(chg->thermal_hi_volt_icl_ua)) {
		smblib_dbg(chg, PR_MISC, "thermal icl table is NULL\n");
		goto unlock;
	}

	if (lv > chg->thermal_lo_volt_icl_levels - 1 ||
	    lv > chg->thermal_hi_volt_icl_levels - 1) {
		smblib_dbg(chg, PR_MISC, "thermal lv is out of range\n");
		goto unlock;
	}

	if (chg->thermal_lo_volt_icl_levels !=
				chg->thermal_hi_volt_icl_levels) {
		smblib_dbg(chg, PR_MISC, "thermal table size missmatch\n");
		goto unlock;
	}

	type = chg->real_charger_type;
	if (type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_CHANGE_STATUS rc=%d\n", rc);
			goto unlock;
		}

		switch (stat & QC_2P0_STATUS_MASK) {
		case QC_5V_BIT:
			icl = chg->thermal_lo_volt_icl_ua[lv];
			break;
		case QC_9V_BIT:
		case QC_12V_BIT:
			icl = chg->thermal_hi_volt_icl_ua[lv];
			break;
		default:
			icl = chg->thermal_lo_volt_icl_ua[lv];
			break;
		}
		smblib_dbg(chg, PR_MISC, "QC2.0: icl=%duA\n", icl);
	} else if (type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
		rc = smblib_read(chg, QC_PULSE_COUNT_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			goto unlock;
		}
		pulses = (stat & QC_PULSE_COUNT_MASK);

		if (pulses >= QC3_PULSES_FOR_6V)
			icl = chg->thermal_hi_volt_icl_ua[lv];
		else
			icl = chg->thermal_lo_volt_icl_ua[lv];

		smblib_dbg(chg, PR_MISC, "QC3.0: icl=%duA\n", icl);
	} else if (type == POWER_SUPPLY_TYPE_USB_PD) {
		if (chg->voltage_max_uv >= 6000000)
			icl = chg->thermal_hi_volt_icl_ua[lv];
		else
			icl = chg->thermal_lo_volt_icl_ua[lv];

		smblib_dbg(chg, PR_MISC, "PD: icl=%duA\n", icl);
#if !defined(CONFIG_ARCH_SONY_TAMA)
	} else if (type == POWER_SUPPLY_TYPE_USB) {
		if (chg->thermal_lo_volt_icl_ua[lv] == 0)
			icl = 0;
		else if (chg->thermal_lo_volt_icl_ua[lv] < USBIN_150MA)
			icl = USBIN_100MA;
		else if (chg->thermal_lo_volt_icl_ua[lv] < USBIN_500MA)
			icl = USBIN_150MA;
		else if (chg->thermal_lo_volt_icl_ua[lv] < USBIN_900MA)
			icl = USBIN_500MA;
		else
			icl = USBIN_900MA;

		if (chg->thermal_lo_volt_icl_ua[lv] != icl)
			smblib_dbg(chg, PR_MISC, "Round icl for SDP %d to %d\n",
					chg->thermal_lo_volt_icl_ua[lv], icl);

		smblib_dbg(chg, PR_MISC, "SDP: icl=%duA\n", icl);
#endif
	} else {
		icl = chg->thermal_lo_volt_icl_ua[lv];
		smblib_dbg(chg, PR_MISC, "DCP/SDP/Other: icl=%duA\n", icl);
	}

	vote(chg->usb_icl_votable, THERMAL_DAEMON_VOTER, true, icl);

#if defined(CONFIG_ARCH_SONY_TAMA)
	if (chg->wireless_enable) {
		if (!IS_ERR_OR_NULL(chg->thermal_wireless_v_limit)) {
			cancel_delayed_work_sync(&chg->wireless_v_chg_work);
			schedule_delayed_work(&chg->wireless_v_chg_work,
							msecs_to_jiffies(0));
		}

		if (IS_ERR_OR_NULL(chg->thermal_dcin_icl_ua)) {
			smblib_dbg(chg, PR_MISC,
					"thermal dcin icl table is NULL\n");
			goto unlock;
		}

		if (lv > chg->thermal_dcin_icl_levels - 1) {
			smblib_dbg(chg, PR_MISC,
					"thermal dcin lv is out of range\n");
			goto unlock;
		}

		if (chg->thermal_hi_volt_icl_levels !=
			chg->thermal_dcin_icl_levels) {
			smblib_dbg(chg, PR_MISC,
					"thermal dcin table size missmatch\n");
			goto unlock;
		}

		icl = chg->thermal_dcin_icl_ua[lv];
		smblib_dbg(chg, PR_MISC, "DCIN: icl=%duA\n", icl);
		vote(chg->dc_icl_votable, THERMAL_DAEMON_VOTER, true, icl);
	}
#endif

unlock:
	mutex_unlock(&chg->thermal_lock);
	return;
}

void smblib_somc_set_low_batt_suspend_en(struct smb_charger *chg)
{
	int rc;

	rc = vote(chg->usb_icl_votable, LOW_BATT_EN_VOTER, true, 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't set usb suspend rc %d\n", rc);

	rc = vote(chg->dc_suspend_votable, LOW_BATT_EN_VOTER, true, 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't set dc suspend rc %d\n", rc);
}

#if defined(CONFIG_ARCH_SONY_TAMA)
#define INHIBIT_HOLD_MSOC 100
void smblib_somc_ctrl_inhibit(struct smb_charger *chg, bool en)
{
	int rc;
	int msoc;
	bool fv_decreased;
	union power_supply_propval val = {0, };

	if (!chg->bms_psy) {
		smblib_err(chg, "Couldn't get bms_psy\n");
		return;
	}

	rc = power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_MONOTONIC_SOC, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get prop msoc rc=%d\n", rc);
		return;
	}
	msoc = val.intval;

	if (chg->batt_profile_fv_uv < chg->last_batt_profile_fv_uv)
		fv_decreased = true;
	else
		fv_decreased = false;

	smblib_dbg(chg, PR_MISC, "msoc:%d, FV:%d->%d\n",
				msoc, chg->last_batt_profile_fv_uv,
				chg->batt_profile_fv_uv);

	if (en) {
		if (fv_decreased && (msoc == INHIBIT_HOLD_MSOC)) {
			smblib_dbg(chg, PR_SOMC, "Enable inhibit for RB WA\n");
			rc = smblib_masked_write(chg, CHGR_CFG2_REG,
							CHARGER_INHIBIT_BIT,
							CHARGER_INHIBIT_BIT);
			if (rc < 0) {
				dev_err(chg->dev,
					"Couldn't set inhibit rc=%d\n", rc);
				return;
			}
		} else {
			smblib_dbg(chg, PR_MISC, "Don't enable inhibit\n");
		}
	} else {
		if (msoc < INHIBIT_HOLD_MSOC) {
			smblib_dbg(chg, PR_SOMC, "Disable inhibit for RB WA\n");
			rc = smblib_masked_write(chg, CHGR_CFG2_REG,
							CHARGER_INHIBIT_BIT, 0);
			if (rc < 0) {
				dev_err(chg->dev,
					"Couldn't set inhibit rc=%d\n", rc);
				return;
			}
		} else {
			smblib_dbg(chg, PR_MISC, "Don't disable inhibit\n");
		}
	}
	chg->last_batt_profile_fv_uv = chg->batt_profile_fv_uv;
}
#endif

const char *smblib_somc_get_battery_charger_status(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg,
			  "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n", rc);
		return ERROR_CHARGE_TYPE;
	}
	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (stat > DISABLE_CHARGE)
		return ERROR_CHARGE_TYPE;
	else
		return smblib_charge_type_name[stat];
}

#define FULL_CAPACITY		100
#define DECIMAL_CEIL		100
int smblib_somc_lrc_get_capacity(struct smb_charger *chg, int capacity)
{
	int ceil, magni;

	if (chg->lrc_fake_capacity &&
		chg->lrc_enabled && chg->lrc_socmax) {
		magni = FULL_CAPACITY * DECIMAL_CEIL / chg->lrc_socmax;
		capacity *= magni;
		ceil = (capacity % DECIMAL_CEIL) ? 1 : 0;
		capacity = capacity / DECIMAL_CEIL + ceil;
		if (capacity > FULL_CAPACITY)
			capacity = FULL_CAPACITY;
	}
	return capacity;
}

void smblib_somc_lrc_vote(struct smb_charger *chg, enum somc_lrc_status status)
{
	int rc;

	if (status == LRC_CHG_OFF)
		rc = vote(chg->chg_disable_votable, BATTCHG_LRC_EN_VOTER,
			true, 0);
	else
		rc = vote(chg->chg_disable_votable, BATTCHG_LRC_EN_VOTER,
			false, 0);

	if (rc < 0)
		dev_err(chg->dev,
			"Couldn't vote for battchg suspend: rc = %d\n", rc);
}

void smblib_somc_lrc_check(struct smb_charger *chg)
{
	int rc, soc = 0;
	enum somc_lrc_status retcode = LRC_DISABLE;
	union power_supply_propval val = {0, };

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0 || !val.intval)
		goto exit;

	if (chg->lrc_enabled) {
		if (chg->lrc_socmax <= chg->lrc_socmin) {
			pr_err("invalid SOC min:%d max:%d\n", chg->lrc_socmin,
							chg->lrc_socmax);
			goto exit;
		}
	} else {
		if (chg->lrc_status == LRC_CHG_OFF)
			smblib_somc_lrc_vote(chg, LRC_CHG_ON);
		goto exit;
	}

	if (chg->bms_psy) {
		rc = power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_CAPACITY, &val);
		if (rc) {
			pr_err("Couldn't get soc rc = %d\n", rc);
			goto exit;
		} else {
			soc = val.intval;
		}
	}

	if (soc >= (chg->lrc_socmax + chg->lrc_hysterisis))
		retcode = LRC_CHG_OFF;
	else if (soc <= chg->lrc_socmin)
		retcode = LRC_CHG_ON;
	else if (chg->lrc_status == LRC_CHG_OFF)
		retcode = LRC_CHG_OFF;
	else
		retcode = LRC_CHG_ON;

	if (retcode != chg->lrc_status)
		smblib_somc_lrc_vote(chg, retcode);

	chg->lrc_status = retcode;

	if (chg->lrc_fake_capacity && soc > chg->lrc_socmax)
		vote(chg->usb_icl_votable, LRC_OVER_SOC_EN_VOTER, true, 0);
	else
		vote(chg->usb_icl_votable, LRC_OVER_SOC_EN_VOTER, false, 0);
	return;

exit:
	chg->lrc_status = LRC_DISABLE;
	return;
}

#define SMART_CHARGE_WDOG_DELAY_MS      (30 * 60 * 1000) /* 30min */
int smblib_somc_smart_set_suspend(struct smb_charger *chg)
{
	int rc = 0;

	if (!chg->smart_charge_enabled) {
		pr_err("Couldn't set smart charge voter due to unactivated\n");
		goto exit;
	}

	rc = vote(chg->chg_disable_votable, BATTCHG_SMART_EN_VOTER,
						chg->smart_charge_suspended, 0);
	if (rc < 0) {
		pr_err("Couldn't vote en rc %d\n", rc);
		goto exit;
	}

	smblib_dbg(chg, PR_SOMC, "voted for smart charging (%d).\n",
					chg->smart_charge_suspended);
	cancel_delayed_work_sync(&chg->smart_charge_wdog_work);
	if (chg->smart_charge_suspended) {
		schedule_delayed_work(&chg->smart_charge_wdog_work,
			msecs_to_jiffies(SMART_CHARGE_WDOG_DELAY_MS));
	}
exit:
	return rc;
}

int smblib_get_usb_max_current_limited(struct smb_charger *chg)
{
	int rc;
	u8 reg;
	rc = smblib_read(chg, USBIN_ICL_OPTIONS_REG, &reg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_ICL_OPTIONS_REG rc=%d\n", rc);
		return 0;
	}

	reg &= (CFG_USB3P0_SEL_BIT | USB51_MODE_BIT);

	switch (reg) {
	case 0:
		rc = USBIN_100MA / 1000;
		break;
	case CFG_USB3P0_SEL_BIT:
		rc = USBIN_150MA / 1000;
		break;
	case USB51_MODE_BIT:
		rc = USBIN_500MA / 1000;
		break;
	case (CFG_USB3P0_SEL_BIT | USB51_MODE_BIT):
		rc = USBIN_900MA / 1000;
		break;
	default:
		rc = 0;
		break;
	}
	return rc;
}

void smblib_somc_handle_jeita_step_fcc(struct smb_charger *chg, int val)
{
	if (val > 0) {
		smblib_dbg(chg, PR_MISC, "set fcc:%d for JEITA/Step\n", val);
		vote(chg->fcc_votable, SOMC_JEITA_STEP_VOTER, true, val);
		vote(chg->chg_disable_votable, SOMC_JEITA_STEP_VOTER, false, 0);
	} else {
		smblib_dbg(chg, PR_MISC, "disable charge for JEITA/Step\n");
		vote(chg->chg_disable_votable, SOMC_JEITA_STEP_VOTER, true, 0);
		vote(chg->fcc_votable, SOMC_JEITA_STEP_VOTER, true, 0);
	}
	chg->step_fcc_ua = val;
}

#define FV_JEITA_WARM_RB_WA_EXIT_THRESH_UV	200000
#define JEITA_FAKE_CARGING_TIME_MS		500
void smblib_somc_handle_jeita_step_fv(struct smb_charger *chg, int val)
{
	union power_supply_propval pval = {0, };
	int rc;
	u8 reg;
	u8 chg_stat;
	bool usbin_valid = false;
	int vbatt;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &reg);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}
	usbin_valid = (bool)(reg & USBIN_PLUGIN_RT_STS_BIT);

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);

	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read VBATT rc=%d\n", rc);
		return;
	}
	vbatt = pval.intval;

	smblib_dbg(chg, PR_SOMC, "set fv:%d\n", val);
	vote(chg->fv_votable, SOMC_JEITA_STEP_VOTER, true, val);

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &chg_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read charger status 1 rc=%d\n", rc);
		return;
	}

	/* WA for Reverse Boost */
	if (!chg->jeita_rb_warm_hi_vbatt_en && !chg->profile_fv_rb_en &&
				chg->jeita_condition == TEMP_CONDITION_WARM &&
				vbatt > val && usbin_valid) {
		smblib_dbg(chg, PR_SOMC,
				"WA for RB after Warm. vbatt=%d\n",
				vbatt);
		chg->jeita_rb_warm_hi_vbatt_en = true;
		vote(chg->usb_icl_votable, SOMC_JEITA_STEP_VOTER, true, 0);
	} else if (chg->jeita_rb_warm_hi_vbatt_en && (!usbin_valid ||
			chg->step_fv_ua < val ||
			vbatt < val - FV_JEITA_WARM_RB_WA_EXIT_THRESH_UV)) {
		smblib_dbg(chg, PR_SOMC,
				"Release WA for RB after Warm. vbatt=%d\n",
				vbatt);
		vote(chg->usb_icl_votable, SOMC_JEITA_STEP_VOTER, false, 0);
		chg->jeita_rb_warm_hi_vbatt_en = false;
	}

	/* WA for holding Charge Termination after normal */
	if (usbin_valid && val > chg->step_fv_ua &&
	    !get_effective_result(chg->chg_disable_votable) &&
	    ((chg_stat & BATTERY_CHARGER_STATUS_MASK) == TERMINATE_CHARGE ||
	    (chg_stat & BATTERY_CHARGER_STATUS_MASK) == INHIBIT_CHARGE ||
	    (chg_stat & CC_SOFT_TERMINATE_BIT) == CC_SOFT_TERMINATE_BIT)) {
		smblib_dbg(chg, PR_SOMC, "Execute WA for holding FULL\n");
		chg->jeita_keep_fake_charging = true;
		vote(chg->chg_disable_votable, SOMC_JEITA_STEP_RECHG_VOTER,
								true, 0);
		vote(chg->chg_disable_votable, SOMC_JEITA_STEP_RECHG_VOTER,
								false, 0);
		msleep(JEITA_FAKE_CARGING_TIME_MS);

		smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &reg);
		smblib_dbg(chg, PR_SOMC, "waiting done chg_sts1=0x%02x\n", reg);
		chg->jeita_keep_fake_charging = false;
	}

	chg->step_fv_ua = val;
}

#define FV_JEITA_WARM_RB_WA_EXIT_THRESH_UV	200000
#define JEITA_FAKE_CARGING_TIME_MS		500
void smblib_somc_handle_profile_fv(struct smb_charger *chg, int val)
{
	bool fv_decreased = false;

	if (val < chg->batt_profile_fv_uv)
		fv_decreased = true;
	else
		fv_decreased = false;

	if (fv_decreased)
		smblib_somc_handle_profile_fv_rb(chg, true);

	smblib_dbg(chg, PR_SOMC, "set fv:%d\n", val);
	vote(chg->fv_votable, BATT_PROFILE_VOTER, true, val);

	chg->batt_profile_fv_uv = val;
}

#define PROFILE_RB_HOLD_MSOC	100
void smblib_somc_handle_profile_fv_rb(struct smb_charger *chg, bool en)
{
	union power_supply_propval pval = {0, };
	int rc;
	u8 reg;
	bool usbin_valid = false;
	int msoc;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &reg);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}
	usbin_valid = (bool)(reg & USBIN_PLUGIN_RT_STS_BIT);

	if (!chg->bms_psy)
		return;

	rc = power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_MONOTONIC_SOC, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get prop msoc rc=%d\n", rc);
		return;
	}
	msoc = pval.intval;

	/* WA for Reverse Boost */
	if (!chg->profile_fv_rb_en && en &&
		usbin_valid && msoc == PROFILE_RB_HOLD_MSOC) {
		smblib_dbg(chg, PR_SOMC,
				"WA for RB with profile fv drop\n");
		chg->profile_fv_rb_en = true;
		vote(chg->usb_icl_votable, SOMC_PROFILE_RB_VOTER, true, 0);
	} else if (chg->profile_fv_rb_en && msoc < PROFILE_RB_HOLD_MSOC) {
		smblib_dbg(chg, PR_SOMC,
				"Release WA for RB with profile fv drop\n");
		vote(chg->usb_icl_votable, SOMC_PROFILE_RB_VOTER, false, 0);
		chg->profile_fv_rb_en = false;
	}
}

void smblib_somc_set_prop_jeita_condition(struct smb_charger *chg,
					const union power_supply_propval *val)
{
	int temp_condition = chg->jeita_condition;

	chg->jeita_condition = val->intval;
	if (chg->jeita_condition != temp_condition)
		power_supply_changed(chg->batt_psy);
}

#if defined(CONFIG_ARCH_SONY_TAMA)
void smblib_somc_handle_wireless_exclusion(struct smb_charger *chg)
{
	union power_supply_propval val = {0, };
	int rc;
	bool usb_present;

	if (!chg->wireless_enable)
		return;

	if (!chg->wireless_psy)
		chg->wireless_psy = power_supply_get_by_name("wireless");

	if (!chg->wireless_psy) {
		smblib_err(chg, "will not set wireless property\n");
		return;
	}

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get present rc=%d\n", rc);
		return;
	}
	usb_present = val.intval;

	smblib_dbg(chg, PR_MISC,
			"usb_present:%d, vbus_reg_en:%d, running_status:%d\n",
			(int)usb_present, (int)chg->vbus_reg_en,
			chg->running_status);

	if (usb_present || chg->vbus_reg_en)
		val.intval = 1;
	else
		val.intval = 0;

	power_supply_set_property(chg->wireless_psy,
					POWER_SUPPLY_PROP_USBIN_VALID, &val);
}
#endif
#endif
