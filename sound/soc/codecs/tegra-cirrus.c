/*
 * ASoC machine driver for Cirrus Logic Audio Card
 * (with CS47L35 codec)
 *
 * Copyright 2015-2017 Matthias Reichl <hias@horus.com>
 *
 * Based on rpi-cirrus-sound-pi driver (c) Wolfson / Cirrus Logic Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <sound/pcm_params.h>

#include <linux/mfd/madera/registers.h>
#include <linux/mfd/madera/core.h>

#include "madera.h"

#define CS47L35_MAX_SYSCLK_1 98304000 /* max sysclk */
#define AUD_MCLK 12288000 //256X of the sampling frequency i.e 256x48000

enum {
	DAI_CS47L35 = 0,
};

struct tegra_cirrus_priv {
	/* mutex for synchronzing FLL1 access with DAPM */
	struct mutex lock;
};

/* helper functions */
static inline struct snd_soc_pcm_runtime *get_cs47l35_runtime(
	struct snd_soc_card *card) {
	//struct snd_soc_pcm_runtime *snd_soc_get_pcm_runtime(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link)
	return snd_soc_get_pcm_runtime(card, card->dai_link[DAI_CS47L35].name);
}

const struct snd_soc_dapm_widget tegra_cirrus_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Headphone", NULL),
	// SND_SOC_DAPM_SPK("d1 Headphone", NULL),
	// SND_SOC_DAPM_SPK("d2 Headphone", NULL),
	// SND_SOC_DAPM_SPK("d3 Headphone", NULL),
};

const struct snd_soc_dapm_route tegra_cirrus_dapm_routes[] = {
	//{ sink, control, source }
	{ "Headphone", NULL, "HPOUTL" },
    { "Headphone", NULL, "HPOUTR" },
};

static int tegra_cirrus_clear_flls(struct snd_soc_card *card,
	struct snd_soc_codec *cs47l35_codec) {

	int ret;

	ret = snd_soc_codec_set_pll(cs47l35_codec,
		MADERA_FLL1_REFCLK, MADERA_FLL_SRC_NONE, 0, 0);
		
	if (ret) {
		dev_warn(card->dev,
			"setting FLL1_REFCLK to zero failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static int tegra_cirrus_set_fll(struct snd_soc_card *card,
	struct snd_soc_codec *cs47l35_codec, unsigned int clk_freq)
{
	int ret = snd_soc_codec_set_pll(cs47l35_codec,
		MADERA_FLL1_REFCLK, MADERA_CLK_SRC_MCLK1,
		AUD_MCLK, clk_freq);
	
	if (ret)
		dev_err(card->dev, "Failed to set FLL1 to %d: %d\n",
			clk_freq, ret);

	usleep_range(1000, 2000);
	return ret;
}

static int tegra_cirrus_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	struct tegra_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *cs47l35_runtime = get_cs47l35_runtime(card);
	struct snd_soc_codec *cs47l35_codec = cs47l35_runtime->codec;

	int ret = 0;
	unsigned int clk_freq;

	if (dapm->dev != cs47l35_runtime->codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level == SND_SOC_BIAS_ON)
			break;

		mutex_lock(&priv->lock);

		clk_freq = CS47L35_MAX_SYSCLK_1;
		ret = tegra_cirrus_set_fll(card, cs47l35_codec, clk_freq);
		
		if (ret)
			dev_err(card->dev,
				"set_bias: Failed to set FLL1\n");
			
		mutex_unlock(&priv->lock);
		break;
	
	default:
		break;
	}

	return ret;
}

static int tegra_cirrus_set_bias_level_post(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	struct tegra_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *cs47l35_runtime = get_cs47l35_runtime(card);
	struct snd_soc_codec *cs47l35_codec = cs47l35_runtime->codec;

	if (dapm->dev != cs47l35_runtime->codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		mutex_lock(&priv->lock);

		if (tegra_cirrus_clear_flls(card, cs47l35_codec))
			dev_err(card->dev,
				"set_bias_post: failed to clear FLLs\n");

		mutex_unlock(&priv->lock);

		break;
	
	default:
		break;
	}

	return 0;
}

static struct snd_soc_dai_link tegra_cirrus_dai[] = {
	[DAI_CS47L35] = {
		.name		= "CS47L35",
		.stream_name	= "CS47L35 AiFi",
		.codec_dai_name	= "cs47l35-aif1",
		.codec_name	= "cs47l35-codec",
		.dai_fmt	=   SND_SOC_DAIFMT_I2S
				  | SND_SOC_DAIFMT_NB_NF
				  | SND_SOC_DAIFMT_CBM_CFM,
	},
};


static int tegra_cirrus_late_probe(struct snd_soc_card *card)
{
	struct tegra_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *cs47l35_runtime = get_cs47l35_runtime(card);
	struct snd_soc_codec *cs47l35_codec = cs47l35_runtime->codec;

	int ret, ret1;
	unsigned int clk_freq = CS47L35_MAX_SYSCLK_1;

	//snd_soc_dai_set_sysclk - configure DAI system or master clock.
	ret = snd_soc_dai_set_sysclk(
		cs47l35_runtime->codec_dai, MADERA_CLK_SYSCLK_1, 0, 0);
	if (ret) {
		dev_err(card->dev,
			"Failed to set CS47L35 codec dai clk domain: %d\n", ret);
		return ret;
	}

	return ret;

	//snd_soc_codec_set_sysclk - configure CODEC system or master clock.
	ret1 = snd_soc_codec_set_sysclk(cs47l35_codec,
		MADERA_CLK_SYSCLK_1,
		MADERA_CLK_SRC_FLL1,
		clk_freq,
		SND_SOC_CLOCK_IN);
	if (ret1) {
		dev_err(card->dev, "Failed to set SYSCLK: %d\n", ret);
		goto out;
	}

	out:
		mutex_unlock(&priv->lock);

		return ret1;
}


/* audio machine driver */
static struct snd_soc_card tegra_cirrus_card = {
	.name			= "tegra-Cirrus",
	.driver_name		= "tegraCirrus",
	.owner			= THIS_MODULE,
	.dai_link		= tegra_cirrus_dai,
	.num_links		= ARRAY_SIZE(tegra_cirrus_dai),
	.late_probe		= tegra_cirrus_late_probe,
	.dapm_widgets		= tegra_cirrus_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tegra_cirrus_dapm_widgets),
	.dapm_routes		= tegra_cirrus_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(tegra_cirrus_dapm_routes),
	.set_bias_level		= tegra_cirrus_set_bias_level,
	.set_bias_level_post	= tegra_cirrus_set_bias_level_post,
};

static int tegra_cirrus_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct tegra_cirrus_priv *priv;
	struct device_node *i2s_node;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);

	snd_soc_card_set_drvdata(&tegra_cirrus_card, priv);

	if (!pdev->dev.of_node)
		return -ENODEV;

	i2s_node = of_parse_phandle(
			pdev->dev.of_node, "i2s-controller", 0);
	if (!i2s_node) {
		dev_err(&pdev->dev, "i2s-controller missing in DT\n");
		return -ENODEV;
	}

	tegra_cirrus_dai[DAI_CS47L35].cpu_of_node = i2s_node;
	tegra_cirrus_dai[DAI_CS47L35].platform_of_node = i2s_node;

	tegra_cirrus_card.dev = &pdev->dev;

	ret = devm_snd_soc_register_card(&pdev->dev, &tegra_cirrus_card);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			dev_dbg(&pdev->dev,
				"register card requested probe deferral\n");
		else
			dev_err(&pdev->dev,
				"Failed to register card: %d\n", ret);
	}

	return ret;
}

static const struct of_device_id tegra_cirrus_of_match[] = {
	{ .compatible = "nvidia,tegra-audio-t186ref-mobile-rt565x", },
	{},
};
MODULE_DEVICE_TABLE(of, tegra_cirrus_of_match);

static struct platform_driver tegra_cirrus_driver = {
	.driver	= {
		.name   = "snd-tegra-cirrus",
		.of_match_table = of_match_ptr(tegra_cirrus_of_match),
	},
	.probe	= tegra_cirrus_probe,
};

module_platform_driver(tegra_cirrus_driver);

MODULE_AUTHOR("Matthias Reichl <hias@horus.com>");
MODULE_DESCRIPTION("ASoC driver for Cirrus Logic Audio Card");
MODULE_LICENSE("GPL");
