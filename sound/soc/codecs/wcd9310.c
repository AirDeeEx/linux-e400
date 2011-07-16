/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/mfd/wcd9310/core.h>
#include <linux/mfd/wcd9310/registers.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include "wcd9310.h"

static const DECLARE_TLV_DB_SCALE(digital_gain, 0, 1, 0);
static const DECLARE_TLV_DB_SCALE(line_gain, 0, 7, 1);
static const DECLARE_TLV_DB_SCALE(analog_gain, 0, 25, 1);

enum tabla_bandgap_type {
	TABLA_BANDGAP_OFF = 0,
	TABLA_BANDGAP_AUDIO_MODE,
	TABLA_BANDGAP_MBHC_MODE,
};

struct tabla_priv { /* member undecided */
	struct snd_soc_codec *codec;
	u32 ref_cnt;
	u32 adc_count;
	enum tabla_bandgap_type bandgap_type;
	bool clock_active;
	bool config_mode_active;
	bool mbhc_polling_active;

	struct tabla_mbhc_calibration *calibration;

	struct snd_soc_jack *jack;
};

static int tabla_codec_enable_charge_pump(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %d\n", __func__, event);
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if ((tabla->bandgap_type != TABLA_BANDGAP_AUDIO_MODE) ||
			(!tabla->clock_active)) {
			pr_err("%s: Error, Tabla must have clocks enabled for "
				"charge pump\n", __func__);
			return -EINVAL;
		}

		snd_soc_update_bits(codec, TABLA_A_CP_EN, 0x01, 0x01);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL, 0x01,
			0x01);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLSG_CTL, 0x08, 0x08);
		usleep_range(200, 200);
		snd_soc_update_bits(codec, TABLA_A_CP_STATIC, 0x10, 0x00);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_RESET_CTL, 0x10,
			0x10);
		usleep_range(20, 20);
		snd_soc_update_bits(codec, TABLA_A_CP_STATIC, 0x08, 0x08);
		snd_soc_update_bits(codec, TABLA_A_CP_STATIC, 0x10, 0x10);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLSG_CTL, 0x08, 0x00);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL, 0x01,
			0x00);
		snd_soc_update_bits(codec, TABLA_A_CP_STATIC, 0x08, 0x00);
		snd_soc_update_bits(codec, TABLA_A_CP_EN, 0x01, 0x00);
		break;
	}
	return 0;
}

static const struct snd_kcontrol_new tabla_snd_controls[] = {
	SOC_SINGLE_TLV("LINEOUT1 Volume", TABLA_A_RX_LINE_1_GAIN, 0, 12, 1,
		line_gain),
	SOC_SINGLE_TLV("LINEOUT3 Volume", TABLA_A_RX_LINE_3_GAIN, 0, 12, 1,
		line_gain),
	SOC_SINGLE_TLV("HPHL Volume", TABLA_A_RX_HPH_L_GAIN, 0, 12, 1,
		line_gain),
	SOC_SINGLE_TLV("HPHR Volume", TABLA_A_RX_HPH_R_GAIN, 0, 12, 1,
		line_gain),

	SOC_SINGLE_TLV("RX1 Digital Volume", TABLA_A_CDC_RX1_VOL_CTL_B2_CTL, 0,
		100, 0, digital_gain),
	SOC_SINGLE_TLV("RX2 Digital Volume", TABLA_A_CDC_RX2_VOL_CTL_B2_CTL, 0,
		100, 0, digital_gain),

	SOC_SINGLE_TLV("DEC5 Volume", TABLA_A_CDC_TX5_VOL_CTL_GAIN, 0, 100, 0,
		digital_gain),
	SOC_SINGLE_TLV("DEC6 Volume", TABLA_A_CDC_TX6_VOL_CTL_GAIN, 0, 100, 0,
		digital_gain),

	SOC_SINGLE_TLV("ADC1 Volume", TABLA_A_TX_1_2_EN, 1, 3, 0, analog_gain),
	SOC_SINGLE_TLV("ADC2 Volume", TABLA_A_TX_1_2_EN, 5, 3, 0, analog_gain),

	SOC_SINGLE("MICBIAS1 CAPLESS Switch", TABLA_A_MICB_1_CTL, 4, 1, 1),
};

static const char *rx_mix1_text[] = {
	"ZERO", "SRC1", "SRC2", "IIR1", "IIR2", "RX1", "RX2", "RX3", "RX4",
		"RX5", "RX6", "RX7"
};

static const char *sb_tx1_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC1"
};

static const char *sb_tx5_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC5"
};

static const char *sb_tx6_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC6"
};

static const char const *sb_tx7_to_tx10_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
		"DEC9", "DEC10"
};

static const char *dec1_mux_text[] = {
	"ZERO", "DMIC1", "ADC6",
};

static const char *dec5_mux_text[] = {
	"ZERO", "DMIC5", "ADC2",
};

static const char *dec6_mux_text[] = {
	"ZERO", "DMIC6", "ADC1",
};

static const char const *dec7_mux_text[] = {
	"ZERO", "DMIC1", "DMIC6", "ADC1", "ADC6", "ANC1_FB", "ANC2_FB",
};

static const char *iir1_inp1_text[] = {
	"ZERO", "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
	"DEC9", "DEC10", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7"
};

static const struct soc_enum rx_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX1_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx2_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX2_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx3_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX3_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx4_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX4_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx5_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX5_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum sb_tx5_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B5_CTL, 0, 9, sb_tx5_mux_text);

static const struct soc_enum sb_tx6_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B6_CTL, 0, 9, sb_tx6_mux_text);

static const struct soc_enum sb_tx7_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B7_CTL, 0, 18,
			sb_tx7_to_tx10_mux_text);

static const struct soc_enum sb_tx8_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B8_CTL, 0, 18,
			sb_tx7_to_tx10_mux_text);

static const struct soc_enum sb_tx1_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B1_CTL, 0, 9, sb_tx1_mux_text);

static const struct soc_enum dec1_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B1_CTL, 0, 3, dec1_mux_text);

static const struct soc_enum dec5_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B2_CTL, 0, 3, dec5_mux_text);

static const struct soc_enum dec6_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B2_CTL, 2, 3, dec6_mux_text);

static const struct soc_enum dec7_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B2_CTL, 4, 7, dec7_mux_text);

static const struct soc_enum iir1_inp1_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_EQ1_B1_CTL, 0, 18, iir1_inp1_text);

static const struct snd_kcontrol_new rx_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX1 MIX1 INP1 Mux", rx_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx2_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX2 MIX1 INP1 Mux", rx2_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx3_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX3 MIX1 INP1 Mux", rx3_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx4_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX4 MIX1 INP1 Mux", rx4_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx5_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX5 MIX1 INP1 Mux", rx5_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new sb_tx5_mux =
	SOC_DAPM_ENUM("SLIM TX5 MUX Mux", sb_tx5_mux_enum);

static const struct snd_kcontrol_new sb_tx6_mux =
	SOC_DAPM_ENUM("SLIM TX6 MUX Mux", sb_tx6_mux_enum);

static const struct snd_kcontrol_new sb_tx7_mux =
	SOC_DAPM_ENUM("SLIM TX7 MUX Mux", sb_tx7_mux_enum);

static const struct snd_kcontrol_new sb_tx8_mux =
	SOC_DAPM_ENUM("SLIM TX8 MUX Mux", sb_tx8_mux_enum);

static const struct snd_kcontrol_new sb_tx1_mux =
	SOC_DAPM_ENUM("SLIM TX1 MUX Mux", sb_tx1_mux_enum);

static const struct snd_kcontrol_new dec1_mux =
	SOC_DAPM_ENUM("DEC1 MUX Mux", dec1_mux_enum);

static const struct snd_kcontrol_new dec5_mux =
	SOC_DAPM_ENUM("DEC5 MUX Mux", dec5_mux_enum);

static const struct snd_kcontrol_new dec6_mux =
	SOC_DAPM_ENUM("DEC6 MUX Mux", dec6_mux_enum);

static const struct snd_kcontrol_new dec7_mux =
	SOC_DAPM_ENUM("DEC7 MUX Mux", dec7_mux_enum);

static const struct snd_kcontrol_new iir1_inp1_mux =
	SOC_DAPM_ENUM("IIR1 INP1 Mux", iir1_inp1_mux_enum);

static const struct snd_kcontrol_new dac1_control =
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_EAR_EN, 5, 1, 0);

static const struct snd_kcontrol_new hphl_switch =
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_HPH_L_DAC_CTL, 6, 1, 0);

static const struct snd_kcontrol_new hphr_switch =
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_HPH_R_DAC_CTL, 6, 1, 0);

static const struct snd_kcontrol_new lineout1_switch =
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_LINE_1_DAC_CTL, 6, 1, 0);

static const struct snd_kcontrol_new lineout3_switch =
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_LINE_3_DAC_CTL, 6, 1, 0);

static void tabla_codec_enable_adc_block(struct snd_soc_codec *codec,
	int enable)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %d\n", __func__, enable);

	if (enable) {
		tabla->adc_count++;
		snd_soc_update_bits(codec, TABLA_A_TX_COM_BIAS, 0xE0, 0xE0);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL, 0x2, 0x2);
	} else {
		tabla->adc_count--;
		if (!tabla->adc_count) {
			snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL,
				0x2, 0x0);
			if (!tabla->mbhc_polling_active)
				snd_soc_update_bits(codec, TABLA_A_TX_COM_BIAS,
					0xE0, 0x0);
		}
	}
}

static int tabla_codec_enable_adc(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u16 adc_reg;

	pr_debug("%s %d\n", __func__, event);

	if (w->reg == TABLA_A_TX_1_2_EN)
		adc_reg = TABLA_A_TX_1_2_TEST_CTL;
	else if (w->reg == TABLA_A_TX_3_4_EN)
		adc_reg = TABLA_A_TX_3_4_TEST_CTL;
	else if (w->reg == TABLA_A_TX_5_6_EN)
		adc_reg = TABLA_A_TX_5_6_TEST_CTL;
	else {
		pr_err("%s: Error, invalid adc register\n", __func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		tabla_codec_enable_adc_block(codec, 1);
		break;
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(codec, adc_reg, 1 << w->shift,
			1 << w->shift);
		usleep_range(1000, 1000);
		snd_soc_update_bits(codec, adc_reg, 1 << w->shift, 0x00);
		usleep_range(1000, 1000);
		break;
	case SND_SOC_DAPM_POST_PMD:
		tabla_codec_enable_adc_block(codec, 0);
		break;
	}
	return 0;
}

static int tabla_codec_enable_pamp_gain(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	pr_debug("%s %d\n", __func__, event);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_RX_EAR_GAIN, 0x80, 0x80);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, TABLA_A_RX_EAR_GAIN, 0x80, 0x00);
		break;
	}
	return 0;
}

static int tabla_codec_enable_lineout(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u16 lineout_gain_reg;

	pr_debug("%s %d\n", __func__, event);

	switch (w->shift) {
	case 0:
		lineout_gain_reg = TABLA_A_RX_LINE_1_GAIN;
		break;
	case 1:
		lineout_gain_reg = TABLA_A_RX_LINE_2_GAIN;
		break;
	case 2:
		lineout_gain_reg = TABLA_A_RX_LINE_3_GAIN;
		break;
	case 3:
		lineout_gain_reg = TABLA_A_RX_LINE_4_GAIN;
		break;
	case 4:
		lineout_gain_reg = TABLA_A_RX_LINE_5_GAIN;
		break;
	default:
		pr_err("%s: Error, incorrect lineout register value\n",
			__func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, lineout_gain_reg, 0x40, 0x40);
		break;
	case SND_SOC_DAPM_POST_PMU:
		usleep_range(40000, 40000);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, lineout_gain_reg, 0x40, 0x00);
		break;
	}
	return 0;
}

static int tabla_codec_enable_dmic1(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	pr_debug("%s %d\n", __func__, event);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_TX1_MUX_CTL, 0x1, 0x1);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_DMIC_CTL, 0x2, 0x2);
		snd_soc_update_bits(codec, TABLA_A_CDC_TX1_DMIC_CTL, 0x1, 0x1);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_DMIC_CTL, 0x1, 0x1);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, TABLA_A_CDC_TX1_DMIC_CTL, 0x1, 0);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_DMIC_CTL, 0x3, 0);
		break;
	}
	return 0;
}

static int tabla_codec_enable_micbias(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u16 micb_cfilt_reg, micb_int_reg;
	char *internal_text = "Internal";

	pr_debug("%s %d\n", __func__, event);
	switch (w->reg) {
	case TABLA_A_MICB_1_CTL:
		micb_cfilt_reg = TABLA_A_MICB_CFILT_1_CTL;
		micb_int_reg = TABLA_A_MICB_1_INT_RBIAS;
		break;
	case TABLA_A_MICB_2_CTL:
		micb_cfilt_reg = TABLA_A_MICB_CFILT_2_CTL;
		micb_int_reg = TABLA_A_MICB_2_INT_RBIAS;
		break;
	case TABLA_A_MICB_3_CTL:
		micb_cfilt_reg = TABLA_A_MICB_CFILT_3_CTL;
		micb_int_reg = TABLA_A_MICB_3_INT_RBIAS;
		break;
	default:
		pr_err("%s: Error, invalid micbias register\n", __func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, micb_cfilt_reg, 0x80, 0x80);
		if (strnstr(w->name, internal_text, 20))
			snd_soc_update_bits(codec, micb_int_reg, 0xE0, 0xE0);
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (strnstr(w->name, internal_text, 20))
			snd_soc_update_bits(codec, micb_int_reg, 0x80, 0x00);
		snd_soc_update_bits(codec, micb_cfilt_reg, 0x80, 0);
		break;
	}

	return 0;
}

static int tabla_codec_enable_dec_clock(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	pr_debug("%s %d\n", __func__, event);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL, 0x4, 0x4);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL, 0x4, 0x0);
		break;
	}
	return 0;
}

static int tabla_codec_reset_interpolator_1(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x1,
			0x1);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x1,
			0x0);
		break;
	}
	return 0;
}

static int tabla_codec_reset_interpolator_2(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x2,
			0x2);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x2,
			0x0);
		break;
	}
	return 0;
}

static int tabla_codec_reset_interpolator_3(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x4,
			0x4);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x4,
			0x0);
		break;
	}
	return 0;
}

static int tabla_codec_reset_interpolator_4(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x8,
			0x8);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x8,
			0x0);
		break;
	}
	return 0;
}

static int tabla_codec_reset_interpolator_5(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x10,
			0x10);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL, 0x10,
			0x0);
		break;
	}
	return 0;
}

static const struct snd_soc_dapm_widget tabla_dapm_widgets[] = {
	/*RX stuff */
	SND_SOC_DAPM_OUTPUT("EAR"),

	SND_SOC_DAPM_PGA_E("EAR PA", TABLA_A_RX_EAR_EN, 4, 0, NULL, 0,
		tabla_codec_enable_pamp_gain, SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_PGA("EAR PA Input", TABLA_A_CDC_CLSG_CTL, 2, 0, NULL, 0),

	SND_SOC_DAPM_SWITCH("DAC1", TABLA_A_RX_EAR_EN, 6, 0, &dac1_control),
	SND_SOC_DAPM_PGA_E("RX1 CP", SND_SOC_NOPM, 0, 0, NULL, 0,
		tabla_codec_enable_charge_pump, SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA("RX BIAS", TABLA_A_RX_COM_BIAS, 7, 0, NULL, 0),
	SND_SOC_DAPM_MUX_E("RX1 MIX1 INP1", TABLA_A_CDC_CLK_RX_B1_CTL, 0, 0,
		&rx_mix1_inp1_mux, tabla_codec_reset_interpolator_1,
		SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_AIF_IN("SLIM RX1", "AIF1 Playback", 0,
		TABLA_A_CDC_RX1_B6_CTL, 5, 0),

	/* RX 2 path */
	SND_SOC_DAPM_PGA_E("RX2 CP", SND_SOC_NOPM, 0, 0, NULL, 0,
		tabla_codec_enable_charge_pump, SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_MUX_E("RX2 MIX1 INP1", TABLA_A_CDC_CLK_RX_B1_CTL, 1, 0,
		&rx2_mix1_inp1_mux, tabla_codec_reset_interpolator_2,
		SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_AIF_IN("SLIM RX2", "AIF1 Playback", 0,
		TABLA_A_CDC_RX2_B6_CTL, 5, 0),

	/* Headphone */
	SND_SOC_DAPM_OUTPUT("HEADPHONE"),
	SND_SOC_DAPM_PGA("HPHL", TABLA_A_RX_HPH_CNP_EN, 5, 0, NULL, 0),
	SND_SOC_DAPM_SWITCH("HPHL DAC", TABLA_A_RX_HPH_L_DAC_CTL, 7, 0,
		&hphl_switch),

	SND_SOC_DAPM_PGA("HPHR", TABLA_A_RX_HPH_CNP_EN, 4, 0, NULL, 0),
	SND_SOC_DAPM_SWITCH("HPHR DAC", TABLA_A_RX_HPH_R_DAC_CTL, 7, 0,
		&hphr_switch),

	/* Speaker */
	SND_SOC_DAPM_OUTPUT("LINEOUT"),
	SND_SOC_DAPM_PGA_E("LINEOUT1", TABLA_A_RX_LINE_CNP_EN, 0, 0, NULL, 0,
		tabla_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SWITCH("LINEOUT1 DAC", TABLA_A_RX_LINE_1_DAC_CTL, 7, 0,
		&lineout1_switch),
	SND_SOC_DAPM_MUX_E("RX3 MIX1 INP1", TABLA_A_CDC_CLK_RX_B1_CTL, 2, 0,
		&rx3_mix1_inp1_mux, tabla_codec_reset_interpolator_3,
		SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_PGA_E("LINEOUT3", TABLA_A_RX_LINE_CNP_EN, 2, 0, NULL, 0,
		tabla_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SWITCH("LINEOUT3 DAC", TABLA_A_RX_LINE_3_DAC_CTL, 7, 0,
		&lineout3_switch),
	SND_SOC_DAPM_MUX_E("RX4 MIX1 INP1", TABLA_A_CDC_CLK_RX_B1_CTL, 3, 0,
		&rx4_mix1_inp1_mux, tabla_codec_reset_interpolator_4,
		SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_MUX_E("RX5 MIX1 INP1", TABLA_A_CDC_CLK_RX_B1_CTL, 4, 0,
		&rx5_mix1_inp1_mux, tabla_codec_reset_interpolator_5,
		SND_SOC_DAPM_PRE_PMU),

	/* TX */
	SND_SOC_DAPM_INPUT("AMIC1"),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS1 External", TABLA_A_MICB_1_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS1 Internal", TABLA_A_MICB_1_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC1", NULL, TABLA_A_TX_1_2_EN, 7, 0,
		tabla_codec_enable_adc, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC1 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 0, 0,
		&dec1_mux, tabla_codec_enable_dec_clock,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC5 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 4, 0,
		&dec5_mux, tabla_codec_enable_dec_clock,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC6 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 5, 0,
		&dec6_mux, tabla_codec_enable_dec_clock,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC7 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 6, 0,
		&dec7_mux, tabla_codec_enable_dec_clock,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_INPUT("AMIC2"),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 External", TABLA_A_MICB_2_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 Internal", TABLA_A_MICB_2_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS3 External", TABLA_A_MICB_3_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS3 Internal", TABLA_A_MICB_3_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC2", NULL, TABLA_A_TX_1_2_EN, 3, 0,
		tabla_codec_enable_adc, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX("SLIM TX1 MUX", SND_SOC_NOPM, 0, 0, &sb_tx1_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX1", "AIF1 Capture", NULL, SND_SOC_NOPM,
			0, 0),

	SND_SOC_DAPM_MUX("SLIM TX5 MUX", SND_SOC_NOPM, 0, 0, &sb_tx5_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX5", "AIF1 Capture", NULL, SND_SOC_NOPM,
			4, 0),

	SND_SOC_DAPM_MUX("SLIM TX6 MUX", SND_SOC_NOPM, 0, 0, &sb_tx6_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX6", "AIF1 Capture", NULL, SND_SOC_NOPM,
			5, 0),

	SND_SOC_DAPM_MUX("SLIM TX7 MUX", SND_SOC_NOPM, 0, 0, &sb_tx7_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX7", "AIF1 Capture", NULL, SND_SOC_NOPM,
			0, 0),

	SND_SOC_DAPM_MUX("SLIM TX8 MUX", SND_SOC_NOPM, 0, 0, &sb_tx8_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX8", "AIF1 Capture", NULL, SND_SOC_NOPM,
			0, 0),

	/* Digital Mic */
	SND_SOC_DAPM_INPUT("DMIC1 IN"),
	SND_SOC_DAPM_MIC("DMIC1", &tabla_codec_enable_dmic1),

	/* Sidetone */
	SND_SOC_DAPM_MUX("IIR1 INP1 MUX", SND_SOC_NOPM, 0, 0, &iir1_inp1_mux),
	SND_SOC_DAPM_PGA("IIR1", TABLA_A_CDC_CLK_SD_CTL, 0, 0, NULL, 0),
};

static const struct snd_soc_dapm_route audio_map[] = {
	/* SLIMBUS Connections */
	{"RX BIAS", NULL, "SLIM RX1"},
	{"RX BIAS", NULL, "SLIM RX2"},

	{"SLIM TX1", NULL, "SLIM TX1 MUX"},
	{"SLIM TX1 MUX", "DEC1", "DEC1 MUX"},

	{"SLIM TX5", NULL, "SLIM TX5 MUX"},
	{"SLIM TX5 MUX", "DEC5", "DEC5 MUX"},

	{"SLIM TX6", NULL, "SLIM TX6 MUX"},
	{"SLIM TX6 MUX", "DEC6", "DEC6 MUX"},

	{"SLIM TX7", NULL, "SLIM TX7 MUX"},
	{"SLIM TX7 MUX", "DEC1", "DEC1 MUX"},
	{"SLIM TX7 MUX", "DEC7", "DEC7 MUX"},
	{"SLIM TX7 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX7 MUX", "DEC6", "DEC6 MUX"},

	{"SLIM TX8", NULL, "SLIM TX8 MUX"},
	{"SLIM TX8 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX8 MUX", "DEC6", "DEC6 MUX"},

	/* Earpiece (RX MIX1) */
	{"EAR", NULL, "EAR PA"},
	{"EAR PA", NULL, "EAR PA Input"},
	{"EAR PA Input", NULL, "DAC1"},
	{"DAC1", "Switch", "RX1 CP"},
	{"RX1 CP", NULL, "RX1 MIX1 INP1"},
	{"RX1 MIX1 INP1", "RX1", "RX BIAS"},

	/* Headset (RX MIX1 and RX MIX2) */
	{"HEADPHONE", NULL, "HPHL"},
	{"HPHL", NULL, "HPHL DAC"},
	{"HPHL DAC", "Switch", "RX1 MIX1 INP1"},

	{"HEADPHONE", NULL, "HPHR"},
	{"HPHR", NULL, "HPHR DAC"},
	{"HPHR DAC", "Switch", "RX2 CP"},
	{"RX2 CP", NULL, "RX2 MIX1 INP1"},
	{"RX2 MIX1 INP1", "RX2", "RX BIAS"},

	/* Speaker (RX MIX3 and RX MIX4) */
	{"LINEOUT", NULL, "LINEOUT1"},
	{"LINEOUT1", NULL, "LINEOUT1 DAC"},
	{"LINEOUT1 DAC", "Switch", "RX3 MIX1 INP1"},
	{"RX3 MIX1 INP1", "RX1", "RX BIAS"},

	{"LINEOUT", NULL, "LINEOUT3"},
	{"LINEOUT3", NULL, "LINEOUT3 DAC"},
	{"LINEOUT3 DAC", "Switch", "RX5 MIX1 INP1"},
	{"RX4 MIX1 INP1", "RX2", "RX BIAS"},
	{"RX5 MIX1 INP1", "RX2", "RX BIAS"},

	/* Handset TX */
	{"DEC5 MUX", "ADC2", "ADC2"},
	{"DEC6 MUX", "ADC1", "ADC1"},
	{"ADC1", NULL, "AMIC1"},
	{"ADC2", NULL, "AMIC2"},

	/* Digital Mic */
	{"DEC1 MUX", "DMIC1", "DMIC1"},
	{"DEC7 MUX", "DMIC1", "DMIC1"},
	{"DMIC1", NULL, "DMIC1 IN"},

	/* Sidetone (IIR1) */
	{"RX1 MIX1 INP1", "IIR1", "IIR1"},
	{"IIR1", NULL, "IIR1 INP1 MUX"},
	{"IIR1 INP1 MUX", "DEC6", "DEC6 MUX"},

};

static int tabla_readable(unsigned int reg)
{
	return tabla_reg_readable[reg];
}

static int tabla_volatile(unsigned int reg)
{
	/* Registers lower than 0x100 are top level registers which can be
	 * written by the Tabla core driver.
	 */

	if ((reg >= TABLA_A_CDC_MBHC_EN_CTL) || (reg < 0x100))
		return 1;

	return 0;
}

#define TABLA_FORMATS (SNDRV_PCM_FMTBIT_S16_LE)
static int tabla_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	int ret;
	pr_debug("%s: write reg %x val %x\n", __func__, reg, value);

	BUG_ON(reg > TABLA_MAX_REGISTER);

	if (!tabla_volatile(reg)) {
		pr_debug("writing to cache\n");
		ret = snd_soc_cache_write(codec, reg, value);
		if (ret != 0)
			dev_err(codec->dev, "Cache write to %x failed: %d\n",
				reg, ret);
	}

	return tabla_reg_write(codec->control_data, reg, value);
}
static unsigned int tabla_read(struct snd_soc_codec *codec,
				unsigned int reg)
{
	unsigned int val;
	int ret;

	BUG_ON(reg > TABLA_MAX_REGISTER);

	if (!tabla_volatile(reg) && tabla_readable(reg) &&
		reg < codec->driver->reg_cache_size) {
		pr_debug("reading from cache\n");
		ret = snd_soc_cache_read(codec, reg, &val);
		if (ret >= 0) {
			pr_debug("register %d, value %d\n", reg, val);
			return val;
		} else
			dev_err(codec->dev, "Cache read from %x failed: %d\n",
				reg, ret);
	}

	val = tabla_reg_read(codec->control_data, reg);
	pr_debug("%s: read reg %x val %x\n", __func__, reg, val);
	return val;
}

static void tabla_codec_enable_audio_mode_bandgap(struct snd_soc_codec *codec)
{
	snd_soc_write(codec, TABLA_A_BIAS_REF_CTL, 0x1C);
	snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x80,
		0x80);
	snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x04,
		0x04);
	snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x01,
		0x01);
	usleep_range(1000, 1000);
	snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x80,
		0x00);
}

static void tabla_codec_enable_bandgap(struct snd_soc_codec *codec,
	enum tabla_bandgap_type choice)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	/* TODO lock resources accessed by audio streams and threaded
	 * interrupt handlers
	 */

	pr_debug("%s, choice is %d, current is %d\n", __func__, choice,
		tabla->bandgap_type);

	if (tabla->bandgap_type == choice)
		return;

	if ((tabla->bandgap_type == TABLA_BANDGAP_OFF) &&
		(choice == TABLA_BANDGAP_AUDIO_MODE)) {
		tabla_codec_enable_audio_mode_bandgap(codec);
	} else if ((tabla->bandgap_type == TABLA_BANDGAP_AUDIO_MODE) &&
		(choice == TABLA_BANDGAP_MBHC_MODE)) {
		snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x2,
			0x2);
		snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x80,
			0x80);
		snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x4,
			0x4);
		usleep_range(1000, 1000);
		snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x80,
			0x00);
	} else if ((tabla->bandgap_type == TABLA_BANDGAP_MBHC_MODE) &&
		(choice == TABLA_BANDGAP_AUDIO_MODE)) {
		snd_soc_write(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x00);
		usleep_range(100, 100);
		tabla_codec_enable_audio_mode_bandgap(codec);
	} else if (choice == TABLA_BANDGAP_OFF) {
		snd_soc_write(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x00);
	} else {
		pr_err("%s: Error, Invalid bandgap settings\n", __func__);
	}
	tabla->bandgap_type = choice;
}

static int tabla_codec_enable_config_mode(struct snd_soc_codec *codec,
	int enable)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	if (enable) {
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_FREQ, 0x10, 0);
		snd_soc_write(codec, TABLA_A_BIAS_CONFIG_MODE_BG_CTL, 0x17);
		usleep_range(5, 5);
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_FREQ, 0x80,
			0x80);
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_TEST, 0x80,
			0x80);
		usleep_range(10, 10);
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_TEST, 0x80, 0);
		usleep_range(20, 20);
		snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x08, 0x08);
	} else {
		snd_soc_update_bits(codec, TABLA_A_BIAS_CONFIG_MODE_BG_CTL, 0x1,
			0);
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_FREQ, 0x80, 0);
	}
	tabla->config_mode_active = enable ? true : false;

	return 0;
}

static int tabla_codec_enable_clock_block(struct snd_soc_codec *codec,
	int config_mode)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s\n", __func__);

	if (config_mode) {
		tabla_codec_enable_config_mode(codec, 1);
		snd_soc_write(codec, TABLA_A_CLK_BUFF_EN2, 0x00);
		snd_soc_write(codec, TABLA_A_CLK_BUFF_EN2, 0x02);
		snd_soc_write(codec, TABLA_A_CLK_BUFF_EN1, 0x0D);
		usleep_range(1000, 1000);
	} else
		snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x08, 0x00);

	if (!config_mode && tabla->mbhc_polling_active) {
		snd_soc_write(codec, TABLA_A_CLK_BUFF_EN2, 0x02);
		tabla_codec_enable_config_mode(codec, 0);

	}

	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x05, 0x05);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN2, 0x02, 0x00);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN2, 0x04, 0x04);
	snd_soc_update_bits(codec, TABLA_A_CDC_CLK_MCLK_CTL, 0x01, 0x01);
	usleep_range(50, 50);
	tabla->clock_active = true;
	return 0;
}
static void tabla_codec_disable_clock_block(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	pr_debug("%s\n", __func__);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN2, 0x04, 0x00);
	ndelay(160);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN2, 0x02, 0x02);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x05, 0x00);
	tabla->clock_active = false;
}

static int tabla_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	pr_debug("%s()\n", __func__);

	if (!codec) {
		pr_err("Error, no codec found\n");
		return -EINVAL;
	}
	tabla->ref_cnt++;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		/* Enable LDO */
		snd_soc_update_bits(codec, TABLA_A_MICB_CFILT_1_VAL, 0xFC,
			0xA0);
		snd_soc_update_bits(codec, TABLA_A_LDO_H_MODE_1, 0x80, 0x80);
		usleep_range(1000, 1000);
	}

	if (tabla->mbhc_polling_active && (tabla->ref_cnt == 1)) {
		tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_AUDIO_MODE);
		tabla_codec_enable_clock_block(codec, 0);
	}

	return ret;
}

static void tabla_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s()\n", __func__);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		/* Disable LDO */
		snd_soc_update_bits(codec, TABLA_A_LDO_H_MODE_1, 0x80, 0x00);
		usleep_range(1000, 1000);
	}

	if (!tabla->ref_cnt) {
		pr_err("Error, trying to shutdown codec when already down\n");
		return;
	}
	tabla->ref_cnt--;

	if (tabla->mbhc_polling_active) {
		if (!tabla->ref_cnt) {
			tabla_codec_enable_bandgap(codec,
				TABLA_BANDGAP_MBHC_MODE);
			snd_soc_update_bits(codec, TABLA_A_RX_COM_BIAS, 0x80,
				0x80);
			tabla_codec_enable_clock_block(codec, 1);
		}
		snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x05, 0x01);
	}
}

static int tabla_digital_mute(struct snd_soc_dai *codec_dai, int mute)
{
	struct snd_soc_codec *codec = codec_dai->codec;

	pr_debug("%s %d\n", __func__, mute);

	/* TODO mute TX */
	if (mute)
		snd_soc_update_bits(codec, TABLA_A_CDC_RX1_B6_CTL, 0x01, 0x01);
	else
		snd_soc_update_bits(codec, TABLA_A_CDC_RX1_B6_CTL, 0x01, 0x00);

	return 0;
}

static int tabla_set_dai_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int tabla_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int tabla_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	pr_debug("%s: DAI-ID %x\n", __func__, dai->id);
	return 0;
}

static struct snd_soc_dai_ops tabla_dai_ops = {
	.startup = tabla_startup,
	.shutdown = tabla_shutdown,
	.hw_params = tabla_hw_params,
	.set_sysclk = tabla_set_dai_sysclk,
	.set_fmt = tabla_set_dai_fmt,
	.digital_mute = tabla_digital_mute,
};

static struct snd_soc_dai_driver tabla_dai[] = {
	{
		.name = "tabla_rx1",
		.id = 1,
		.playback = {
			.stream_name = "AIF1 Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = TABLA_FORMATS,
			.rate_max = 48000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &tabla_dai_ops,
	},
	{
		.name = "tabla_tx1",
		.id = 2,
		.capture = {
			.stream_name = "AIF1 Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = TABLA_FORMATS,
			.rate_max = 48000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &tabla_dai_ops,
	},
};

static void tabla_codec_setup_hs_polling(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	struct tabla_mbhc_calibration *calibration = tabla->calibration;
	int micbias_ctl_reg, micbias_cfilt_val_reg, micbias_cfilt_ctl_reg;

	if (!calibration) {
		pr_err("Error, no tabla calibration\n");
		return;
	}

	tabla->mbhc_polling_active = true;

	if (!tabla->ref_cnt) {
		tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_MBHC_MODE);
		snd_soc_update_bits(codec, TABLA_A_RX_COM_BIAS, 0x80, 0x80);
		tabla_codec_enable_clock_block(codec, 1);
	}

	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x05, 0x01);

	/* TODO store register values in calibration */
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B4_CTL, 0x09);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B3_CTL, 0xEE);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B2_CTL, 0xFC);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B1_CTL, 0xCE);

	snd_soc_update_bits(codec, TABLA_A_LDO_H_MODE_1, 0x0F, 0x0D);
	snd_soc_update_bits(codec, TABLA_A_TX_COM_BIAS, 0xE0, 0xE0);

	/* TODO select cfilt separately from the micbias line inside the machine
	 * driver
	 */
	switch (calibration->bias) {
	case TABLA_MICBIAS1:
		micbias_ctl_reg = TABLA_A_MICB_1_CTL;
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_1_CTL;
		micbias_cfilt_val_reg = TABLA_A_MICB_CFILT_1_VAL;
		break;
	case TABLA_MICBIAS2:
		micbias_ctl_reg = TABLA_A_MICB_2_CTL;
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_2_CTL;
		micbias_cfilt_val_reg = TABLA_A_MICB_CFILT_2_VAL;
		break;
	case TABLA_MICBIAS3:
		micbias_ctl_reg = TABLA_A_MICB_3_CTL;
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_3_CTL;
		micbias_cfilt_val_reg = TABLA_A_MICB_CFILT_3_VAL;
		break;
	case TABLA_MICBIAS4:
		pr_err("%s: Error, microphone bias 4 not supported\n",
			__func__);
		return;
	default:
		pr_err("Error, invalid mic bias line\n");
		return;
	}
	snd_soc_write(codec, micbias_cfilt_ctl_reg, 0x40);

	snd_soc_write(codec, micbias_ctl_reg, 0x36);

	snd_soc_write(codec, micbias_cfilt_val_reg, 0x68);

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x2, 0x2);
	snd_soc_write(codec, TABLA_A_MBHC_SCALING_MUX_1, 0x4);

	snd_soc_update_bits(codec, TABLA_A_TX_7_MBHC_EN, 0x80, 0x80);
	snd_soc_update_bits(codec, TABLA_A_TX_7_MBHC_EN, 0x1F, 0x1C);
	snd_soc_update_bits(codec, TABLA_A_TX_7_MBHC_TEST_CTL, 0x40, 0x40);

	snd_soc_update_bits(codec, TABLA_A_TX_7_MBHC_EN, 0x80, 0x00);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x80, 0x80);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x80, 0x00);

	snd_soc_write(codec, TABLA_A_CDC_MBHC_TIMER_B1_CTL, 3);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_TIMER_B2_CTL, 9);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_TIMER_B3_CTL, 30);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_TIMER_B6_CTL, 120);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_TIMER_B1_CTL, 0x78, 0x58);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_B2_CTL, 11);

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_B1_CTL, 0x4, 0x4);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x8);

	tabla_enable_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL);
	tabla_enable_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x1);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x0);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x1);
	/* TODO check if we need to maintain the other register bits */
}

static int tabla_codec_enable_hs_detect(struct snd_soc_codec *codec,
		int insertion)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	struct tabla_mbhc_calibration *calibration = tabla->calibration;
	int central_bias_enabled = 0;
	int micbias_int_reg, micbias_ctl_reg, micbias_mbhc_reg;

	if (!calibration) {
		pr_err("Error, no tabla calibration\n");
		return -EINVAL;
	}

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_INT_CTL, 0x1, 0);

	if (insertion)
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_INT_CTL, 0x2, 0);
	else
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_INT_CTL, 0x2, 0x2);

	if (snd_soc_read(codec, TABLA_A_CDC_MBHC_B1_CTL) & 0x4) {
		if (!(tabla->clock_active)) {
			tabla_codec_enable_config_mode(codec, 1);
			snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_B1_CTL,
				0x04, 0);
			usleep_range(calibration->shutdown_plug_removal,
				calibration->shutdown_plug_removal);
			tabla_codec_enable_config_mode(codec, 0);
		} else
			snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_B1_CTL,
				0x04, 0);
	}

	snd_soc_update_bits(codec, TABLA_A_MBHC_HPH, 0xC,
		calibration->hph_current << 2);

	snd_soc_update_bits(codec, TABLA_A_MBHC_HPH, 0x13, 0x13);

	switch (calibration->bias) {
	case TABLA_MICBIAS1:
		micbias_mbhc_reg = TABLA_A_MICB_1_MBHC;
		micbias_int_reg = TABLA_A_MICB_1_INT_RBIAS;
		micbias_ctl_reg = TABLA_A_MICB_1_CTL;
		break;
	case TABLA_MICBIAS2:
		micbias_mbhc_reg = TABLA_A_MICB_2_MBHC;
		micbias_int_reg = TABLA_A_MICB_2_INT_RBIAS;
		micbias_ctl_reg = TABLA_A_MICB_2_CTL;
		break;
	case TABLA_MICBIAS3:
		micbias_mbhc_reg = TABLA_A_MICB_3_MBHC;
		micbias_int_reg = TABLA_A_MICB_3_INT_RBIAS;
		micbias_ctl_reg = TABLA_A_MICB_3_CTL;
		break;
	case TABLA_MICBIAS4:
		micbias_mbhc_reg = TABLA_A_MICB_4_MBHC;
		micbias_int_reg = TABLA_A_MICB_4_INT_RBIAS;
		micbias_ctl_reg = TABLA_A_MICB_4_CTL;
		break;
	default:
		pr_err("Error, invalid mic bias line\n");
		return -EINVAL;
	}
	snd_soc_update_bits(codec, micbias_int_reg, 0x80, 0);
	snd_soc_update_bits(codec, micbias_ctl_reg, 0x1, 0);

	/* If central bandgap disabled */
	if (!(snd_soc_read(codec, TABLA_A_PIN_CTL_OE1) & 1)) {
		snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE1, 0x3, 0x3);
		usleep_range(calibration->bg_fast_settle,
			calibration->bg_fast_settle);
		central_bias_enabled = 1;
	}

	/* If LDO_H disabled */
	if (snd_soc_read(codec, TABLA_A_PIN_CTL_OE0) & 0x80) {
		snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE0, 0x10, 0);
		snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE0, 0x80, 0x80);
		usleep_range(calibration->tldoh, calibration->tldoh);
		snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE0, 0x80, 0);

		if (central_bias_enabled)
			snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE1, 0x1, 0);
	}
	snd_soc_update_bits(codec, micbias_mbhc_reg, 0x60,
		calibration->mic_current << 5);
	snd_soc_update_bits(codec, micbias_mbhc_reg, 0x80, 0x80);
	usleep_range(calibration->mic_pid, calibration->mic_pid);
	snd_soc_update_bits(codec, micbias_mbhc_reg, 0x10, 0x10);

	snd_soc_update_bits(codec, TABLA_A_MICB_4_MBHC, 0x3, calibration->bias);

	tabla_enable_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_INT_CTL, 0x1, 0x1);
	return 0;
}

int tabla_hs_detect(struct snd_soc_codec *codec, struct snd_soc_jack *jack,
	struct tabla_mbhc_calibration *calibration)
{
	struct tabla_priv *tabla;
	if (!codec || !calibration) {
		pr_err("Error: no codec or calibration\n");
		return -EINVAL;
	}
	tabla = snd_soc_codec_get_drvdata(codec);
	tabla->jack = jack;
	tabla->calibration = calibration;

	return tabla_codec_enable_hs_detect(codec, 1);
}
EXPORT_SYMBOL_GPL(tabla_hs_detect);

static irqreturn_t tabla_dummy_handler(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x1);
	return IRQ_HANDLED;
}

static irqreturn_t tabla_hs_insert_irq(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;

	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION);

	if (priv->jack) {
		pr_debug("reporting insertion %d\n", SND_JACK_HEADSET);
		snd_soc_jack_report(priv->jack, SND_JACK_HEADSET,
			SND_JACK_HEADSET);
	}
	usleep_range(priv->calibration->setup_plug_removal_delay,
		priv->calibration->setup_plug_removal_delay);

	tabla_codec_setup_hs_polling(codec);

	return IRQ_HANDLED;
}

static void tabla_codec_shutdown_hs_polling(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	if (!tabla->ref_cnt) {
		snd_soc_update_bits(codec, TABLA_A_TX_COM_BIAS, 0xE0, 0x00);
		tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_AUDIO_MODE);
		tabla_codec_enable_clock_block(codec, 0);
	}

	tabla->mbhc_polling_active = false;
}

static irqreturn_t tabla_hs_remove_irq(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;

	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL);
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL);

	usleep_range(priv->calibration->shutdown_plug_removal,
		priv->calibration->shutdown_plug_removal);

	if (priv->jack) {
		pr_debug("reporting removal\n");
		snd_soc_jack_report(priv->jack, 0, SND_JACK_HEADSET);
	}
	tabla_codec_shutdown_hs_polling(codec);

	tabla_codec_enable_hs_detect(codec, 1);

	return IRQ_HANDLED;
}

static unsigned long slimbus_value;

static irqreturn_t tabla_slimbus_irq(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	int i, j;
	u8 val;

	for (i = 0; i < TABLA_SLIM_NUM_PORT_REG; i++) {
		slimbus_value = tabla_interface_reg_read(codec->control_data,
			TABLA_SLIM_PGD_PORT_INT_STATUS0 + i);
		for_each_set_bit(j, &slimbus_value, BITS_PER_BYTE) {
			val = tabla_interface_reg_read(codec->control_data,
				TABLA_SLIM_PGD_PORT_INT_SOURCE0 + i*8 + j);
			if (val & 0x1)
				pr_err_ratelimited("overflow error on port %x,"
					" value %x\n", i*8 + j, val);
			if (val & 0x2)
				pr_err_ratelimited("underflow error on port %x,"
					" value %x\n", i*8 + j, val);
		}
		tabla_interface_reg_write(codec->control_data,
			TABLA_SLIM_PGD_PORT_INT_CLR0 + i, 0xFF);
	}

	return IRQ_HANDLED;
}

static int tabla_codec_probe(struct snd_soc_codec *codec)
{
	struct tabla *control;
	struct tabla_priv *tabla;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	int ret = 0;
	int i;
	int tx_channel;

	codec->control_data = dev_get_drvdata(codec->dev->parent);
	control = codec->control_data;

	tabla = kzalloc(sizeof(struct tabla_priv), GFP_KERNEL);
	if (!tabla) {
		dev_err(codec->dev, "Failed to allocate private data\n");
		return -ENOMEM;
	}

	snd_soc_codec_set_drvdata(codec, tabla);

	tabla->ref_cnt = 0;
	tabla->bandgap_type = TABLA_BANDGAP_OFF;
	tabla->clock_active = false;
	tabla->config_mode_active = false;
	tabla->mbhc_polling_active = false;
	tabla->codec = codec;

	/* TODO only enable bandgap when necessary in order to save power */
	tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_AUDIO_MODE);
	tabla_codec_enable_clock_block(codec, 0);

	/* Initialize gain registers to use register gain */
	snd_soc_update_bits(codec, TABLA_A_RX_HPH_L_GAIN, 0x10, 0x10);
	snd_soc_update_bits(codec, TABLA_A_RX_HPH_R_GAIN, 0x10, 0x10);
	snd_soc_update_bits(codec, TABLA_A_RX_LINE_1_GAIN, 0x10, 0x10);
	snd_soc_update_bits(codec, TABLA_A_RX_LINE_3_GAIN, 0x10, 0x10);

	/* Initialize mic biases to differential mode */
	snd_soc_update_bits(codec, TABLA_A_MICB_1_INT_RBIAS, 0x24, 0x24);
	snd_soc_update_bits(codec, TABLA_A_MICB_2_INT_RBIAS, 0x24, 0x24);
	snd_soc_update_bits(codec, TABLA_A_MICB_3_INT_RBIAS, 0x24, 0x24);
	snd_soc_update_bits(codec, TABLA_A_MICB_4_INT_RBIAS, 0x24, 0x24);

	/* Set headset CFILT to fast mode */
	snd_soc_update_bits(codec, TABLA_A_MICB_CFILT_1_CTL, 0x00, 0x00);
	snd_soc_update_bits(codec, TABLA_A_MICB_CFILT_2_CTL, 0x00, 0x00);
	snd_soc_update_bits(codec, TABLA_A_MICB_CFILT_3_CTL, 0x00, 0x00);

	snd_soc_update_bits(codec, TABLA_A_CDC_CONN_CLSG_CTL, 0x30, 0x10);

	/* Use 16 bit sample size for now */
	for (tx_channel = 0; tx_channel < 6; tx_channel++) {
		snd_soc_update_bits(codec,
			TABLA_A_CDC_CONN_TX_SB_B1_CTL + tx_channel, 0x30, 0x20);
		snd_soc_update_bits(codec,
			TABLA_A_CDC_TX1_MUX_CTL + tx_channel, 0x8, 0x0);

	}
	for (tx_channel = 6; tx_channel < 10; tx_channel++) {
		snd_soc_update_bits(codec,
			TABLA_A_CDC_CONN_TX_SB_B1_CTL + tx_channel, 0x60, 0x40);
		snd_soc_update_bits(codec,
			TABLA_A_CDC_TX1_MUX_CTL + tx_channel, 0x8, 0x0);
	}
	snd_soc_write(codec, TABLA_A_CDC_CONN_RX_SB_B1_CTL, 0xAA);
	snd_soc_write(codec, TABLA_A_CDC_CONN_RX_SB_B2_CTL, 0xAA);

	snd_soc_add_controls(codec, tabla_snd_controls,
		ARRAY_SIZE(tabla_snd_controls));
	snd_soc_dapm_new_controls(dapm, tabla_dapm_widgets,
		ARRAY_SIZE(tabla_dapm_widgets));
	snd_soc_dapm_add_routes(dapm, audio_map, ARRAY_SIZE(audio_map));
	snd_soc_dapm_sync(dapm);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION,
		tabla_hs_insert_irq, "Headset insert detect", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_MBHC_INSERTION);
		goto err_insert_irq;
	}
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL,
		tabla_hs_remove_irq, "Headset remove detect", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_MBHC_REMOVAL);
		goto err_remove_irq;
	}
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL,
		tabla_dummy_handler, "DC Estimation detect", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_MBHC_POTENTIAL);
		goto err_potential_irq;
	}
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_SLIMBUS,
		tabla_slimbus_irq, "SLIMBUS Slave", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_SLIMBUS);
		goto err_slimbus_irq;
	}

	for (i = 0; i < TABLA_SLIM_NUM_PORT_REG; i++)
		tabla_interface_reg_write(codec->control_data,
			TABLA_SLIM_PGD_PORT_INT_EN0 + i, 0xFF);

	return ret;

err_slimbus_irq:
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL, tabla);
err_potential_irq:
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL, tabla);
err_remove_irq:
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION, tabla);
err_insert_irq:
	kfree(tabla);
	return ret;
}
static int tabla_codec_remove(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	tabla_free_irq(codec->control_data, TABLA_IRQ_SLIMBUS, tabla);
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL, tabla);
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL, tabla);
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION, tabla);
	tabla_codec_disable_clock_block(codec);
	tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_OFF);
	kfree(tabla);
	return 0;
}
static struct snd_soc_codec_driver soc_codec_dev_tabla = {
	.probe	= tabla_codec_probe,
	.remove	= tabla_codec_remove,
	.read = tabla_read,
	.write = tabla_write,

	.readable_register = tabla_readable,
	.volatile_register = tabla_volatile,

	.reg_cache_size = TABLA_CACHE_SIZE,
	.reg_cache_default = tabla_reg_defaults,
	.reg_word_size = 1,
};
static int __devinit tabla_probe(struct platform_device *pdev)
{
	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_tabla,
		tabla_dai, ARRAY_SIZE(tabla_dai));
}
static int __devexit tabla_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}
static struct platform_driver tabla_codec_driver = {
	.probe = tabla_probe,
	.remove = tabla_remove,
	.driver = {
		.name = "tabla_codec",
		.owner = THIS_MODULE,
	},
};

static int __init tabla_codec_init(void)
{
	return platform_driver_register(&tabla_codec_driver);
}

static void __exit tabla_codec_exit(void)
{
	platform_driver_unregister(&tabla_codec_driver);
}

module_init(tabla_codec_init);
module_exit(tabla_codec_exit);

MODULE_DESCRIPTION("Tabla codec driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL v2");
