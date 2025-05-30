// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 */
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/nvmem-consumer.h>
#include <linux/etherdevice.h>
#include "mt76.h"

static int mt76_get_of_eeprom_data(struct mt76_dev *dev, void *eep, int len)
{
	struct device_node *np = dev->dev->of_node;
	const void *data;
	int size;

	data = of_get_property(np, "mediatek,eeprom-data", &size);
	if (!data)
		return -ENOENT;

	if (size > len)
		return -EINVAL;

	memcpy(eep, data, size);

	return 0;
}

int mt76_get_of_data_from_mtd(struct mt76_dev *dev, void *eep, int offset, int len)
{
#ifdef CONFIG_MTD
	struct device_node *np = dev->dev->of_node;
	struct mtd_info *mtd;
	const __be32 *list;
	const char *part;
	phandle phandle;
	size_t retlen;
	int size;
	int ret;

	list = of_get_property(np, "mediatek,mtd-eeprom", &size);
	if (!list)
		return -ENOENT;

	phandle = be32_to_cpup(list++);
	if (!phandle)
		return -ENOENT;

	np = of_find_node_by_phandle(phandle);
	if (!np)
		return -EINVAL;

	part = of_get_property(np, "label", NULL);
	if (!part)
		part = np->name;

	mtd = get_mtd_device_nm(part);
	if (IS_ERR(mtd)) {
		ret =  PTR_ERR(mtd);
		goto out_put_node;
	}

	if (size <= sizeof(*list)) {
		ret = -EINVAL;
		goto out_put_node;
	}

	offset += be32_to_cpup(list);
	ret = mtd_read(mtd, offset, len, &retlen, eep);
	put_mtd_device(mtd);
	if (mtd_is_bitflip(ret))
		ret = 0;
	if (ret) {
		dev_err(dev->dev, "reading EEPROM from mtd %s failed: %i\n",
			part, ret);
		goto out_put_node;
	}

	if (retlen < len) {
		ret = -EINVAL;
		goto out_put_node;
	}

	if (of_property_read_bool(dev->dev->of_node, "big-endian")) {
		u8 *data = (u8 *)eep;
		int i;

		/* convert eeprom data in Little Endian */
		for (i = 0; i < round_down(len, 2); i += 2)
			put_unaligned_le16(get_unaligned_be16(&data[i]),
					   &data[i]);
	}

#ifdef CONFIG_NL80211_TESTMODE
	if (len == dev->eeprom.size) {
		dev->test_mtd.name = devm_kstrdup(dev->dev, part, GFP_KERNEL);
		dev->test_mtd.offset = offset;
	}
#endif

out_put_node:
	of_node_put(np);
	return ret;
#else
	return -ENOENT;
#endif
}
EXPORT_SYMBOL_GPL(mt76_get_of_data_from_mtd);

static void mt76_get_nvmem_part(struct mt76_dev *dev)
{
#ifdef CONFIG_NL80211_TESTMODE
	struct device_node *np = dev->dev->of_node;
	const __be32 *list;
	const char *part;
	phandle phandle;
	int offset, size;
	u32 reg[2];

	list = of_get_property(np, "nvmem-cells", NULL);
	if (!list)
		return;

	phandle = be32_to_cpup(list++);
	if (!phandle)
		return;

	np = of_find_node_by_phandle(phandle);
	if (!np)
		return;

	if (of_property_read_u32_array(np, "reg", reg, 2))
		return;

	offset = reg[0];
	size = reg[1];

	np = of_get_parent(of_get_parent(np));
	if (!np)
		return;

	part = of_get_property(np, "partname", NULL);
	if (!part)
		part = of_get_property(np, "volname", NULL);
	if (!part)
		return;

	if (size != dev->eeprom.size)
		return;

	dev->test_mtd.name = devm_kstrdup(dev->dev, part, GFP_KERNEL);
	dev->test_mtd.offset = offset;
#endif
}

int mt76_get_of_data_from_nvmem(struct mt76_dev *dev, void *eep,
				const char *cell_name, int len)
{
	struct device_node *np = dev->dev->of_node;
	struct nvmem_cell *cell;
	const void *data;
	size_t retlen;
	int ret = 0;

	cell = of_nvmem_cell_get(np, cell_name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	mt76_get_nvmem_part(dev);

	data = nvmem_cell_read(cell, &retlen);
	nvmem_cell_put(cell);

	if (IS_ERR(data))
		return PTR_ERR(data);

	if (retlen < len) {
		ret = -EINVAL;
		goto exit;
	}

	memcpy(eep, data, len);

exit:
	kfree(data);

	return ret;
}
EXPORT_SYMBOL_GPL(mt76_get_of_data_from_nvmem);

static int mt76_get_of_eeprom(struct mt76_dev *dev, void *eep, int len)
{
	struct device_node *np = dev->dev->of_node;
	int ret;

	if (!np)
		return -ENOENT;

	ret = mt76_get_of_eeprom_data(dev, eep, len);
	if (!ret)
		return 0;

	ret = mt76_get_of_data_from_mtd(dev, eep, 0, len);
	if (!ret)
		return 0;

	return mt76_get_of_data_from_nvmem(dev, eep, "eeprom", len);
}

bool mt76_check_bin_file_mode(struct mt76_dev *dev)
{
	struct device_node *np = dev->dev->of_node;
	const char *bin_file_name = NULL;

	if (!np)
		return false;

	of_property_read_string(np, "bin_file_name", &bin_file_name);

	dev->bin_file_name = bin_file_name;
	if (dev->bin_file_name) {
		dev_info(dev->dev, "Using bin file %s\n", dev->bin_file_name);
#ifdef CONFIG_NL80211_TESTMODE
		dev->test_mtd.name = devm_kstrdup(dev->dev, bin_file_name, GFP_KERNEL);
		dev->test_mtd.offset = -1;
#endif
	}

	return dev->bin_file_name ? true : false;
}
EXPORT_SYMBOL_GPL(mt76_check_bin_file_mode);

void
mt76_eeprom_override(struct mt76_phy *phy)
{
	struct mt76_dev *dev = phy->dev;
	struct device_node *np = dev->dev->of_node, *band_np;
	bool found_mac = false;
	u32 reg;
	int ret;

	for_each_child_of_node(np, band_np) {
		ret = of_property_read_u32(band_np, "reg", &reg);
		if (ret)
			continue;

		if (reg == phy->band_idx) {
			found_mac = !of_get_mac_address(band_np, phy->macaddr);
			break;
		}
	}

	if (!found_mac)
		of_get_mac_address(np, phy->macaddr);

	if (!is_valid_ether_addr(phy->macaddr)) {
		eth_random_addr(phy->macaddr);
		dev_info(dev->dev,
			 "Invalid MAC address, using random address %pM\n",
			 phy->macaddr);
	}
}
EXPORT_SYMBOL_GPL(mt76_eeprom_override);

static bool mt76_string_prop_find(struct property *prop, const char *str)
{
	const char *cp = NULL;

	if (!prop || !str || !str[0])
		return false;

	while ((cp = of_prop_next_string(prop, cp)) != NULL)
		if (!strcasecmp(cp, str))
			return true;

	return false;
}

struct device_node *
mt76_find_power_limits_node(struct mt76_phy *phy)
{
	struct mt76_dev *dev = phy->dev;
	struct device_node *np = dev->dev->of_node;
	const char *const region_names[] = {
		[NL80211_DFS_UNSET] = "ww",
		[NL80211_DFS_ETSI] = "etsi",
		[NL80211_DFS_FCC] = "fcc",
		[NL80211_DFS_JP] = "jp",
	};
	struct device_node *cur, *fallback = NULL;
	const char *region_name = NULL;
	char index[4] = {0};

	if (dev->region < ARRAY_SIZE(region_names))
		region_name = region_names[dev->region];

	np = of_get_child_by_name(np, "power-limits");
	if (!np)
		return NULL;

	snprintf(index, sizeof(index), "%d", phy->sku_idx);
	for_each_child_of_node(np, cur) {
		struct property *country = of_find_property(cur, "country", NULL);
		struct property *regd = of_find_property(cur, "regdomain", NULL);
		struct property *sku_index = of_find_property(cur, "sku-index", NULL);

		if (!country && !regd) {
			fallback = cur;
			continue;
		}

		if (phy->sku_idx && !mt76_string_prop_find(sku_index, index))
			continue;

		if (mt76_string_prop_find(country, dev->alpha2) ||
		    mt76_string_prop_find(regd, region_name)) {
			of_node_put(np);
			return cur;
		}
	}

	of_node_put(np);
	return fallback;
}
EXPORT_SYMBOL_GPL(mt76_find_power_limits_node);

static const __be32 *
mt76_get_of_array(struct device_node *np, char *name, size_t *len, int min)
{
	struct property *prop = of_find_property(np, name, NULL);

	if (!prop || !prop->value || prop->length < min * 4)
		return NULL;

	*len = prop->length;

	return prop->value;
}

struct device_node *
mt76_find_channel_node(struct device_node *np, struct ieee80211_channel *chan)
{
	struct device_node *cur;
	const __be32 *val;
	size_t len;

	for_each_child_of_node(np, cur) {
		val = mt76_get_of_array(cur, "channels", &len, 2);
		if (!val)
			continue;

		while (len >= 2 * sizeof(*val)) {
			if (chan->hw_value >= be32_to_cpu(val[0]) &&
			    chan->hw_value <= be32_to_cpu(val[1]))
				return cur;

			val += 2;
			len -= 2 * sizeof(*val);
		}
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(mt76_find_channel_node);


static s8
mt76_get_txs_delta(struct device_node *np, u8 nss)
{
	const __be32 *val;
	size_t len;

	val = mt76_get_of_array(np, "txs-delta", &len, nss);
	if (!val)
		return 0;

	return be32_to_cpu(val[nss - 1]);
}

static void
mt76_apply_array_limit(s8 *pwr, size_t pwr_len, const __be32 *data,
		       s8 target_power, s8 nss_delta, s8 *max_power)
{
	int i;

	if (!data)
		return;

	for (i = 0; i < pwr_len; i++) {
		pwr[i] = min_t(s8, target_power,
			       (s8)be32_to_cpu(data[i]) + nss_delta);
		*max_power = max(*max_power, pwr[i]);
	}
}

static void
mt76_apply_multi_array_limit(s8 *pwr, size_t pwr_len, s8 pwr_num,
			     const __be32 *data, size_t len, s8 target_power,
			     s8 nss_delta)
{
	int i, cur;
	s8 max_power = -128;

	if (!data)
		return;

	len /= 4;
	cur = be32_to_cpu(data[0]);
	for (i = 0; i < pwr_num; i++) {
		if (len < pwr_len + 1)
			break;

		mt76_apply_array_limit(pwr + pwr_len * i, pwr_len, data + 1,
				       target_power, nss_delta, &max_power);
		if (--cur > 0)
			continue;

		data += pwr_len + 1;
		len -= pwr_len + 1;
		if (!len)
			break;

		cur = be32_to_cpu(data[0]);
	}
}

s8 mt76_get_rate_power_limits(struct mt76_phy *phy,
			      struct ieee80211_channel *chan,
			      struct mt76_power_limits *dest,
			      struct mt76_power_path_limits *dest_path,
			      s8 target_power)
{
	struct mt76_dev *dev = phy->dev;
	struct device_node *np;
	const __be32 *val;
	char name[16];
	u32 mcs_rates = dev->drv->mcs_rates;
	char band;
	size_t len;
	s8 max_power = -127;
	s8 max_power_backoff = -127;
	s8 txs_delta;
	int n_chains = hweight16(phy->chainmask);
	s8 target_power_combine = target_power + mt76_tx_power_path_delta(n_chains);

	if (!mcs_rates)
		mcs_rates = 12;

	memset(dest, target_power, sizeof(*dest));
	if (dest_path != NULL)
		memset(dest_path, 0, sizeof(*dest_path));

	if (!IS_ENABLED(CONFIG_OF))
		return target_power;

	np = mt76_find_power_limits_node(phy);
	if (!np)
		return target_power;

	switch (chan->band) {
	case NL80211_BAND_2GHZ:
		band = '2';
		break;
	case NL80211_BAND_5GHZ:
		band = '5';
		break;
	case NL80211_BAND_6GHZ:
		band = '6';
		break;
	default:
		return target_power;
	}

	snprintf(name, sizeof(name), "txpower-%cg", band);
	np = of_get_child_by_name(np, name);
	if (!np)
		return target_power;

	np = mt76_find_channel_node(np, chan);
	if (!np)
		return target_power;

	txs_delta = mt76_get_txs_delta(np, hweight16(phy->chainmask));

	val = mt76_get_of_array(np, "rates-cck", &len, ARRAY_SIZE(dest->cck));
	mt76_apply_array_limit(dest->cck, ARRAY_SIZE(dest->cck), val,
			       target_power, txs_delta, &max_power);

	val = mt76_get_of_array(np, "rates-ofdm",
				&len, ARRAY_SIZE(dest->ofdm));
	mt76_apply_array_limit(dest->ofdm, ARRAY_SIZE(dest->ofdm), val,
			       target_power, txs_delta, &max_power);

	val = mt76_get_of_array(np, "rates-mcs", &len, mcs_rates + 1);
	mt76_apply_multi_array_limit(dest->mcs[0], ARRAY_SIZE(dest->mcs[0]),
				     ARRAY_SIZE(dest->mcs), val, len,
				     target_power, txs_delta);

	val = mt76_get_of_array(np, "rates-ru", &len, ARRAY_SIZE(dest->ru[0]) + 1);
	mt76_apply_multi_array_limit(dest->ru[0], ARRAY_SIZE(dest->ru[0]),
				     ARRAY_SIZE(dest->ru), val, len,
				     target_power, txs_delta);

	val = mt76_get_of_array(np, "rates-eht", &len, ARRAY_SIZE(dest->eht[0]) + 1);
	mt76_apply_multi_array_limit(dest->eht[0], ARRAY_SIZE(dest->eht[0]),
				     ARRAY_SIZE(dest->eht), val, len,
				     target_power, txs_delta);

	if (dest_path == NULL)
		return max_power;

	max_power_backoff = max_power;

	val = mt76_get_of_array(np, "paths-cck", &len, ARRAY_SIZE(dest_path->cck));
	mt76_apply_array_limit(dest_path->cck, ARRAY_SIZE(dest_path->cck), val,
			       target_power_combine, txs_delta, &max_power_backoff);

	val = mt76_get_of_array(np, "paths-ofdm", &len, ARRAY_SIZE(dest_path->ofdm));
	mt76_apply_array_limit(dest_path->ofdm, ARRAY_SIZE(dest_path->ofdm), val,
			       target_power_combine, txs_delta, &max_power_backoff);

	val = mt76_get_of_array(np, "paths-ofdm-bf", &len, ARRAY_SIZE(dest_path->ofdm_bf));
	mt76_apply_array_limit(dest_path->ofdm_bf, ARRAY_SIZE(dest_path->ofdm_bf), val,
			       target_power_combine, txs_delta, &max_power_backoff);

	val = mt76_get_of_array(np, "paths-ru", &len, ARRAY_SIZE(dest_path->ru[0]) + 1);
	mt76_apply_multi_array_limit(dest_path->ru[0], ARRAY_SIZE(dest_path->ru[0]),
				     ARRAY_SIZE(dest_path->ru), val, len,
				     target_power_combine, txs_delta);

	val = mt76_get_of_array(np, "paths-ru-bf", &len, ARRAY_SIZE(dest_path->ru_bf[0]) + 1);
	mt76_apply_multi_array_limit(dest_path->ru_bf[0], ARRAY_SIZE(dest_path->ru_bf[0]),
				     ARRAY_SIZE(dest_path->ru_bf), val, len,
				     target_power_combine, txs_delta);

	return max_power;
}
EXPORT_SYMBOL_GPL(mt76_get_rate_power_limits);

int
mt76_eeprom_init(struct mt76_dev *dev, int len)
{
	dev->eeprom.size = len;
	dev->eeprom.data = devm_kzalloc(dev->dev, len, GFP_KERNEL);
	if (!dev->eeprom.data)
		return -ENOMEM;

	return !mt76_get_of_eeprom(dev, dev->eeprom.data, len);
}
EXPORT_SYMBOL_GPL(mt76_eeprom_init);
