// SPDX-License-Identifier: ISC
/* Copyright (C) 2020 Felix Fietkau <nbd@nbd.name> */
#ifndef __MT76_TEST_H
#define __MT76_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/nl80211.h>
#include <unl.h>

#include "../testmode.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

#ifndef ETH_ALEN
#define ETH_ALEN	6
#endif

#define EEPROM_FILE_PATH_FMT	"/tmp/mt76-test-%s"
#define EEPROM_PART_SIZE	20480

#define FWLOG_BUF_SIZE	1504

struct nl_msg;
struct nlattr;

#define MT_MAX_BAND	3

struct phy_config {
	int wiphy_idx;
	int radio_idx;
	int radio_num;
	int parking_freq;
	int band;
	uint8_t mac_addr[ETH_ALEN];
};

struct radio_config {
	int parking_freq;
	enum nl80211_band band;
};

struct wiphy_config {
	int wiphy_idx;
	int radio_num;
	struct radio_config radio[MT_MAX_BAND];
};

struct wiphy_list {
	bool is_single_wiphy;
	int wiphy_num;
	uint8_t mac_addr[ETH_ALEN];
	struct wiphy_config wiphy[MT_MAX_BAND * 2];
};

struct tm_field {
	const char *name;
	const char *prefix;

	bool (*parse)(const struct tm_field *field, int idx, struct nl_msg *msg,
		      const char *val);
	void (*print)(const struct tm_field *field, struct nlattr *attr);

	union {
		struct {
			const char * const *enum_str;
			int enum_len;
		};
		struct {
			bool (*parse2)(const struct tm_field *field, int idx,
				       struct nl_msg *msg, const char *val);
			void (*print2)(const struct tm_field *field,
				       struct nlattr *attr);
		};
		struct {
			void (*print_extra)(const struct tm_field *field,
					    struct nlattr **tb);
			const struct tm_field *fields;
			struct nla_policy *policy;
			int len;
		};
	};
};

extern struct unl unl;
extern const struct tm_field msg_field;
extern unsigned char *eeprom_data;

void usage(void);
int mt76_eeprom(int phy, int argc, char **argv);
int mt76_fwlog(const char *phyname, int argc, char **argv);
int mt76_idxlog(const char *phyname, int argc, char **argv);

#endif
