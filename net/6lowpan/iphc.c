/*
 * Copyright 2011, Siemens AG
 * written by Alexander Smirnov <alex.bluesman.smirnov@gmail.com>
 */

/* Based on patches from Jon Smirl <jonsmirl@gmail.com>
 * Copyright (c) 2011 Jon Smirl <jonsmirl@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* Jon's code is based on 6lowpan implementation for Contiki which is:
 * Copyright (c) 2008, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <linux/bitops.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>

#include <net/6lowpan.h>
#include <net/ipv6.h>

/* special link-layer handling */
#include <net/mac802154.h>

#include "nhc.h"

/* Values of fields within the IPHC encoding first byte */
#define LOWPAN_IPHC_TF_MASK	0x18
#define LOWPAN_IPHC_TF_00	0x00
#define LOWPAN_IPHC_TF_01	0x08
#define LOWPAN_IPHC_TF_10	0x10
#define LOWPAN_IPHC_TF_11	0x18

#define LOWPAN_IPHC_NH		0x04

#define LOWPAN_IPHC_HLIM_MASK	0x03
#define LOWPAN_IPHC_HLIM_00	0x00
#define LOWPAN_IPHC_HLIM_01	0x01
#define LOWPAN_IPHC_HLIM_10	0x02
#define LOWPAN_IPHC_HLIM_11	0x03

/* Values of fields within the IPHC encoding second byte */
#define LOWPAN_IPHC_CID		0x80

#define LOWPAN_IPHC_SAC		0x40

#define LOWPAN_IPHC_SAM_MASK	0x30
#define LOWPAN_IPHC_SAM_00	0x00
#define LOWPAN_IPHC_SAM_01	0x10
#define LOWPAN_IPHC_SAM_10	0x20
#define LOWPAN_IPHC_SAM_11	0x30

#define LOWPAN_IPHC_M		0x08

#define LOWPAN_IPHC_DAC		0x04

#define LOWPAN_IPHC_DAM_MASK	0x03
#define LOWPAN_IPHC_DAM_00	0x00
#define LOWPAN_IPHC_DAM_01	0x01
#define LOWPAN_IPHC_DAM_10	0x02
#define LOWPAN_IPHC_DAM_11	0x03

/* ipv6 address based on mac
 * second bit-flip (Universe/Local) is done according RFC2464
 */
#define is_addr_mac_addr_based(a, m) \
	((((a)->s6_addr[8])  == (((m)[0]) ^ 0x02)) &&	\
	 (((a)->s6_addr[9])  == (m)[1]) &&		\
	 (((a)->s6_addr[10]) == (m)[2]) &&		\
	 (((a)->s6_addr[11]) == (m)[3]) &&		\
	 (((a)->s6_addr[12]) == (m)[4]) &&		\
	 (((a)->s6_addr[13]) == (m)[5]) &&		\
	 (((a)->s6_addr[14]) == (m)[6]) &&		\
	 (((a)->s6_addr[15]) == (m)[7]))

/* check whether we can compress the IID to 16 bits,
 * it's possible for unicast addresses with first 49 bits are zero only.
 */
#define lowpan_is_iid_16_bit_compressable(a)	\
	((((a)->s6_addr16[4]) == 0) &&		\
	 (((a)->s6_addr[10]) == 0) &&		\
	 (((a)->s6_addr[11]) == 0xff) &&	\
	 (((a)->s6_addr[12]) == 0xfe) &&	\
	 (((a)->s6_addr[13]) == 0))

/* check whether the 112-bit gid of the multicast address is mappable to: */

/* 48 bits, FFXX::00XX:XXXX:XXXX */
#define lowpan_is_mcast_addr_compressable48(a)	\
	((((a)->s6_addr16[1]) == 0) &&		\
	 (((a)->s6_addr16[2]) == 0) &&		\
	 (((a)->s6_addr16[3]) == 0) &&		\
	 (((a)->s6_addr16[4]) == 0) &&		\
	 (((a)->s6_addr[10]) == 0))

/* 32 bits, FFXX::00XX:XXXX */
#define lowpan_is_mcast_addr_compressable32(a)	\
	((((a)->s6_addr16[1]) == 0) &&		\
	 (((a)->s6_addr16[2]) == 0) &&		\
	 (((a)->s6_addr16[3]) == 0) &&		\
	 (((a)->s6_addr16[4]) == 0) &&		\
	 (((a)->s6_addr16[5]) == 0) &&		\
	 (((a)->s6_addr[12]) == 0))

/* 8 bits, FF02::00XX */
#define lowpan_is_mcast_addr_compressable8(a)	\
	((((a)->s6_addr[1])  == 2) &&		\
	 (((a)->s6_addr16[1]) == 0) &&		\
	 (((a)->s6_addr16[2]) == 0) &&		\
	 (((a)->s6_addr16[3]) == 0) &&		\
	 (((a)->s6_addr16[4]) == 0) &&		\
	 (((a)->s6_addr16[5]) == 0) &&		\
	 (((a)->s6_addr16[6]) == 0) &&		\
	 (((a)->s6_addr[14]) == 0))

static inline void iphc_uncompress_eui64_lladdr(struct in6_addr *ipaddr,
						const void *lladdr)
{
	/* fe:80::XXXX:XXXX:XXXX:XXXX
	 *        \_________________/
	 *              hwaddr
	 */
	ipaddr->s6_addr[0] = 0xFE;
	ipaddr->s6_addr[1] = 0x80;
	memcpy(&ipaddr->s6_addr[8], lladdr, EUI64_ADDR_LEN);
	/* second bit-flip (Universe/Local)
	 * is done according RFC2464
	 */
	ipaddr->s6_addr[8] ^= 0x02;
}

static inline void iphc_uncompress_802154_lladdr(struct in6_addr *ipaddr,
						 const void *lladdr)
{
	const struct ieee802154_addr *addr = lladdr;
	u8 eui64[EUI64_ADDR_LEN] = { };

	switch (addr->mode) {
	case IEEE802154_ADDR_LONG:
		ieee802154_le64_to_be64(eui64, &addr->extended_addr);
		iphc_uncompress_eui64_lladdr(ipaddr, eui64);
		break;
	case IEEE802154_ADDR_SHORT:
		/* fe:80::ff:fe00:XXXX
		 *                \__/
		 *             short_addr
		 *
		 * Universe/Local bit is zero.
		 */
		ipaddr->s6_addr[0] = 0xFE;
		ipaddr->s6_addr[1] = 0x80;
		ipaddr->s6_addr[11] = 0xFF;
		ipaddr->s6_addr[12] = 0xFE;
		ieee802154_le16_to_be16(&ipaddr->s6_addr16[7],
					&addr->short_addr);
		break;
	default:
		/* should never handled and filtered by 802154 6lowpan */
		WARN_ON_ONCE(1);
		break;
	}
}

/* Uncompress address function for source and
 * destination address(non-multicast).
 *
 * address_mode is the masked value for sam or dam value
 */
static int uncompress_addr(struct sk_buff *skb, const struct net_device *dev,
			   struct in6_addr *ipaddr, u8 address_mode,
			   const void *lladdr)
{
	bool fail;

	switch (address_mode) {
	/* SAM and DAM are the same here */
	case LOWPAN_IPHC_DAM_00:
		/* for global link addresses */
		fail = lowpan_fetch_skb(skb, ipaddr->s6_addr, 16);
		break;
	case LOWPAN_IPHC_SAM_01:
	case LOWPAN_IPHC_DAM_01:
		/* fe:80::XXXX:XXXX:XXXX:XXXX */
		ipaddr->s6_addr[0] = 0xFE;
		ipaddr->s6_addr[1] = 0x80;
		fail = lowpan_fetch_skb(skb, &ipaddr->s6_addr[8], 8);
		break;
	case LOWPAN_IPHC_SAM_10:
	case LOWPAN_IPHC_DAM_10:
		/* fe:80::ff:fe00:XXXX */
		ipaddr->s6_addr[0] = 0xFE;
		ipaddr->s6_addr[1] = 0x80;
		ipaddr->s6_addr[11] = 0xFF;
		ipaddr->s6_addr[12] = 0xFE;
		fail = lowpan_fetch_skb(skb, &ipaddr->s6_addr[14], 2);
		break;
	case LOWPAN_IPHC_SAM_11:
	case LOWPAN_IPHC_DAM_11:
		fail = false;
		switch (lowpan_priv(dev)->lltype) {
		case LOWPAN_LLTYPE_IEEE802154:
			iphc_uncompress_802154_lladdr(ipaddr, lladdr);
			break;
		default:
			iphc_uncompress_eui64_lladdr(ipaddr, lladdr);
			break;
		}
		break;
	default:
		pr_debug("Invalid address mode value: 0x%x\n", address_mode);
		return -EINVAL;
	}

	if (fail) {
		pr_debug("Failed to fetch skb data\n");
		return -EIO;
	}

	raw_dump_inline(NULL, "Reconstructed ipv6 addr is",
			ipaddr->s6_addr, 16);

	return 0;
}

/* Uncompress address function for source context
 * based address(non-multicast).
 */
static int uncompress_context_based_src_addr(struct sk_buff *skb,
					     struct in6_addr *ipaddr,
					     u8 address_mode)
{
	switch (address_mode) {
	case LOWPAN_IPHC_SAM_00:
		/* unspec address ::
		 * Do nothing, address is already ::
		 */
		break;
	case LOWPAN_IPHC_SAM_01:
		/* TODO */
	case LOWPAN_IPHC_SAM_10:
		/* TODO */
	case LOWPAN_IPHC_SAM_11:
		/* TODO */
		netdev_warn(skb->dev, "SAM value 0x%x not supported\n",
			    address_mode);
		return -EINVAL;
	default:
		pr_debug("Invalid sam value: 0x%x\n", address_mode);
		return -EINVAL;
	}

	raw_dump_inline(NULL,
			"Reconstructed context based ipv6 src addr is",
			ipaddr->s6_addr, 16);

	return 0;
}

/* Uncompress function for multicast destination address,
 * when M bit is set.
 */
static int lowpan_uncompress_multicast_daddr(struct sk_buff *skb,
					     struct in6_addr *ipaddr,
					     u8 address_mode)
{
	bool fail;

	switch (address_mode) {
	case LOWPAN_IPHC_DAM_00:
		/* 00:  128 bits.  The full address
		 * is carried in-line.
		 */
		fail = lowpan_fetch_skb(skb, ipaddr->s6_addr, 16);
		break;
	case LOWPAN_IPHC_DAM_01:
		/* 01:  48 bits.  The address takes
		 * the form ffXX::00XX:XXXX:XXXX.
		 */
		ipaddr->s6_addr[0] = 0xFF;
		fail = lowpan_fetch_skb(skb, &ipaddr->s6_addr[1], 1);
		fail |= lowpan_fetch_skb(skb, &ipaddr->s6_addr[11], 5);
		break;
	case LOWPAN_IPHC_DAM_10:
		/* 10:  32 bits.  The address takes
		 * the form ffXX::00XX:XXXX.
		 */
		ipaddr->s6_addr[0] = 0xFF;
		fail = lowpan_fetch_skb(skb, &ipaddr->s6_addr[1], 1);
		fail |= lowpan_fetch_skb(skb, &ipaddr->s6_addr[13], 3);
		break;
	case LOWPAN_IPHC_DAM_11:
		/* 11:  8 bits.  The address takes
		 * the form ff02::00XX.
		 */
		ipaddr->s6_addr[0] = 0xFF;
		ipaddr->s6_addr[1] = 0x02;
		fail = lowpan_fetch_skb(skb, &ipaddr->s6_addr[15], 1);
		break;
	default:
		pr_debug("DAM value has a wrong value: 0x%x\n", address_mode);
		return -EINVAL;
	}

	if (fail) {
		pr_debug("Failed to fetch skb data\n");
		return -EIO;
	}

	raw_dump_inline(NULL, "Reconstructed ipv6 multicast addr is",
			ipaddr->s6_addr, 16);

	return 0;
}

/* get the ecn values from iphc tf format and set it to ipv6hdr */
static inline void lowpan_iphc_tf_set_ecn(struct ipv6hdr *hdr, const u8 *tf)
{
	/* get the two higher bits which is ecn */
	u8 ecn = tf[0] & 0xc0;

	/* ECN takes 0x30 in hdr->flow_lbl[0] */
	hdr->flow_lbl[0] |= (ecn >> 2);
}

/* get the dscp values from iphc tf format and set it to ipv6hdr */
static inline void lowpan_iphc_tf_set_dscp(struct ipv6hdr *hdr, const u8 *tf)
{
	/* DSCP is at place after ECN */
	u8 dscp = tf[0] & 0x3f;

	/* The four highest bits need to be set at hdr->priority */
	hdr->priority |= ((dscp & 0x3c) >> 2);
	/* The two lower bits is part of hdr->flow_lbl[0] */
	hdr->flow_lbl[0] |= ((dscp & 0x03) << 6);
}

/* get the flow label values from iphc tf format and set it to ipv6hdr */
static inline void lowpan_iphc_tf_set_lbl(struct ipv6hdr *hdr, const u8 *lbl)
{
	/* flow label is always some array started with lower nibble of
	 * flow_lbl[0] and followed with two bytes afterwards. Inside inline
	 * data the flow_lbl position can be different, which will be handled
	 * by lbl pointer. E.g. case "01" vs "00" the traffic class is 8 bit
	 * shifted, the different lbl pointer will handle that.
	 *
	 * The flow label will started at lower nibble of flow_lbl[0], the
	 * higher nibbles are part of DSCP + ECN.
	 */
	hdr->flow_lbl[0] |= lbl[0] & 0x0f;
	memcpy(&hdr->flow_lbl[1], &lbl[1], 2);
}

/* lowpan_iphc_tf_decompress - decompress the traffic class.
 *	This function will return zero on success, a value lower than zero if
 *	failed.
 */
static int lowpan_iphc_tf_decompress(struct sk_buff *skb, struct ipv6hdr *hdr,
				     u8 val)
{
	u8 tf[4];

	/* Traffic Class and Flow Label */
	switch (val) {
	case LOWPAN_IPHC_TF_00:
		/* ECN + DSCP + 4-bit Pad + Flow Label (4 bytes) */
		if (lowpan_fetch_skb(skb, tf, 4))
			return -EINVAL;

		/*                      1                   2                   3
		 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 * |ECN|   DSCP    |  rsv  |             Flow Label                |
		 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 */
		lowpan_iphc_tf_set_ecn(hdr, tf);
		lowpan_iphc_tf_set_dscp(hdr, tf);
		lowpan_iphc_tf_set_lbl(hdr, &tf[1]);
		break;
	case LOWPAN_IPHC_TF_01:
		/* ECN + 2-bit Pad + Flow Label (3 bytes), DSCP is elided. */
		if (lowpan_fetch_skb(skb, tf, 3))
			return -EINVAL;

		/*                     1                   2
		 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
		 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 * |ECN|rsv|             Flow Label                |
		 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 */
		lowpan_iphc_tf_set_ecn(hdr, tf);
		lowpan_iphc_tf_set_lbl(hdr, &tf[0]);
		break;
	case LOWPAN_IPHC_TF_10:
		/* ECN + DSCP (1 byte), Flow Label is elided. */
		if (lowpan_fetch_skb(skb, tf, 1))
			return -EINVAL;

		/*  0 1 2 3 4 5 6 7
		 * +-+-+-+-+-+-+-+-+
		 * |ECN|   DSCP    |
		 * +-+-+-+-+-+-+-+-+
		 */
		lowpan_iphc_tf_set_ecn(hdr, tf);
		lowpan_iphc_tf_set_dscp(hdr, tf);
		break;
	case LOWPAN_IPHC_TF_11:
		/* Traffic Class and Flow Label are elided */
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	return 0;
}

/* TTL uncompression values */
static const u8 lowpan_ttl_values[] = {
	[LOWPAN_IPHC_HLIM_01] = 1,
	[LOWPAN_IPHC_HLIM_10] = 64,
	[LOWPAN_IPHC_HLIM_11] = 255,
};

int lowpan_header_decompress(struct sk_buff *skb, const struct net_device *dev,
			     const void *daddr, const void *saddr)
{
	struct ipv6hdr hdr = {};
	u8 iphc0, iphc1;
	int err;

	raw_dump_table(__func__, "raw skb data dump uncompressed",
		       skb->data, skb->len);

	if (lowpan_fetch_skb(skb, &iphc0, sizeof(iphc0)) ||
	    lowpan_fetch_skb(skb, &iphc1, sizeof(iphc1)))
		return -EINVAL;

	/* another if the CID flag is set */
	if (iphc1 & LOWPAN_IPHC_CID)
		return -ENOTSUPP;

	hdr.version = 6;

	err = lowpan_iphc_tf_decompress(skb, &hdr,
					iphc0 & LOWPAN_IPHC_TF_MASK);
	if (err < 0)
		return err;

	/* Next Header */
	if (!(iphc0 & LOWPAN_IPHC_NH)) {
		/* Next header is carried inline */
		if (lowpan_fetch_skb(skb, &hdr.nexthdr, sizeof(hdr.nexthdr)))
			return -EINVAL;

		pr_debug("NH flag is set, next header carried inline: %02x\n",
			 hdr.nexthdr);
	}

	/* Hop Limit */
	if ((iphc0 & LOWPAN_IPHC_HLIM_MASK) != LOWPAN_IPHC_HLIM_00) {
		hdr.hop_limit = lowpan_ttl_values[iphc0 & LOWPAN_IPHC_HLIM_MASK];
	} else {
		if (lowpan_fetch_skb(skb, &hdr.hop_limit,
				     sizeof(hdr.hop_limit)))
			return -EINVAL;
	}

	if (iphc1 & LOWPAN_IPHC_SAC) {
		/* Source address context based uncompression */
		pr_debug("SAC bit is set. Handle context based source address.\n");
		err = uncompress_context_based_src_addr(skb, &hdr.saddr,
							iphc1 & LOWPAN_IPHC_SAM_MASK);
	} else {
		/* Source address uncompression */
		pr_debug("source address stateless compression\n");
		err = uncompress_addr(skb, dev, &hdr.saddr,
				      iphc1 & LOWPAN_IPHC_SAM_MASK, saddr);
	}

	/* Check on error of previous branch */
	if (err)
		return -EINVAL;

	/* check for Multicast Compression */
	if (iphc1 & LOWPAN_IPHC_M) {
		if (iphc1 & LOWPAN_IPHC_DAC) {
			pr_debug("dest: context-based mcast compression\n");
			/* TODO: implement this */
		} else {
			err = lowpan_uncompress_multicast_daddr(skb, &hdr.daddr,
								iphc1 & LOWPAN_IPHC_DAM_MASK);

			if (err)
				return -EINVAL;
		}
	} else {
		err = uncompress_addr(skb, dev, &hdr.daddr,
				      iphc1 & LOWPAN_IPHC_DAM_MASK, daddr);
		pr_debug("dest: stateless compression mode %d dest %pI6c\n",
			 iphc1 & LOWPAN_IPHC_DAM_MASK, &hdr.daddr);
		if (err)
			return -EINVAL;
	}

	/* Next header data uncompression */
	if (iphc0 & LOWPAN_IPHC_NH) {
		err = lowpan_nhc_do_uncompression(skb, dev, &hdr);
		if (err < 0)
			return err;
	} else {
		err = skb_cow(skb, sizeof(hdr));
		if (unlikely(err))
			return err;
	}

	switch (lowpan_priv(dev)->lltype) {
	case LOWPAN_LLTYPE_IEEE802154:
		if (lowpan_802154_cb(skb)->d_size)
			hdr.payload_len = htons(lowpan_802154_cb(skb)->d_size -
						sizeof(struct ipv6hdr));
		else
			hdr.payload_len = htons(skb->len);
		break;
	default:
		hdr.payload_len = htons(skb->len);
		break;
	}

	pr_debug("skb headroom size = %d, data length = %d\n",
		 skb_headroom(skb), skb->len);

	pr_debug("IPv6 header dump:\n\tversion = %d\n\tlength  = %d\n\t"
		 "nexthdr = 0x%02x\n\thop_lim = %d\n\tdest    = %pI6c\n",
		hdr.version, ntohs(hdr.payload_len), hdr.nexthdr,
		hdr.hop_limit, &hdr.daddr);

	skb_push(skb, sizeof(hdr));
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_copy_to_linear_data(skb, &hdr, sizeof(hdr));

	raw_dump_table(__func__, "raw header dump", (u8 *)&hdr, sizeof(hdr));

	return 0;
}
EXPORT_SYMBOL_GPL(lowpan_header_decompress);

static const u8 lowpan_iphc_dam_to_sam_value[] = {
	[LOWPAN_IPHC_DAM_00] = LOWPAN_IPHC_SAM_00,
	[LOWPAN_IPHC_DAM_01] = LOWPAN_IPHC_SAM_01,
	[LOWPAN_IPHC_DAM_10] = LOWPAN_IPHC_SAM_10,
	[LOWPAN_IPHC_DAM_11] = LOWPAN_IPHC_SAM_11,
};

static u8 lowpan_compress_addr_64(u8 **hc_ptr, const struct in6_addr *ipaddr,
				  const unsigned char *lladdr, bool sam)
{
	u8 dam = LOWPAN_IPHC_DAM_00;

	if (is_addr_mac_addr_based(ipaddr, lladdr)) {
		dam = LOWPAN_IPHC_DAM_11; /* 0-bits */
		pr_debug("address compression 0 bits\n");
	} else if (lowpan_is_iid_16_bit_compressable(ipaddr)) {
		/* compress IID to 16 bits xxxx::XXXX */
		lowpan_push_hc_data(hc_ptr, &ipaddr->s6_addr16[7], 2);
		dam = LOWPAN_IPHC_DAM_10; /* 16-bits */
		raw_dump_inline(NULL, "Compressed ipv6 addr is (16 bits)",
				*hc_ptr - 2, 2);
	} else {
		/* do not compress IID => xxxx::IID */
		lowpan_push_hc_data(hc_ptr, &ipaddr->s6_addr16[4], 8);
		dam = LOWPAN_IPHC_DAM_01; /* 64-bits */
		raw_dump_inline(NULL, "Compressed ipv6 addr is (64 bits)",
				*hc_ptr - 8, 8);
	}

	if (sam)
		return lowpan_iphc_dam_to_sam_value[dam];
	else
		return dam;
}

/* lowpan_iphc_get_tc - get the ECN + DCSP fields in hc format */
static inline u8 lowpan_iphc_get_tc(const struct ipv6hdr *hdr)
{
	u8 dscp, ecn;

	/* hdr->priority contains the higher bits of dscp, lower are part of
	 * flow_lbl[0]. Note ECN, DCSP is swapped in ipv6 hdr.
	 */
	dscp = (hdr->priority << 2) | ((hdr->flow_lbl[0] & 0xc0) >> 6);
	/* ECN is at the two lower bits from first nibble of flow_lbl[0] */
	ecn = (hdr->flow_lbl[0] & 0x30);
	/* for pretty debug output, also shift ecn to get the ecn value */
	pr_debug("ecn 0x%02x dscp 0x%02x\n", ecn >> 4, dscp);
	/* ECN is at 0x30 now, shift it to have ECN + DCSP */
	return (ecn << 2) | dscp;
}

/* lowpan_iphc_is_flow_lbl_zero - check if flow label is zero */
static inline bool lowpan_iphc_is_flow_lbl_zero(const struct ipv6hdr *hdr)
{
	return ((!(hdr->flow_lbl[0] & 0x0f)) &&
		!hdr->flow_lbl[1] && !hdr->flow_lbl[2]);
}

/* lowpan_iphc_tf_compress - compress the traffic class which is set by
 *	ipv6hdr. Return the corresponding format identifier which is used.
 */
static u8 lowpan_iphc_tf_compress(u8 **hc_ptr, const struct ipv6hdr *hdr)
{
	/* get ecn dscp data in a byteformat as: ECN(hi) + DSCP(lo) */
	u8 tc = lowpan_iphc_get_tc(hdr), tf[4], val;

	/* printout the traffic class in hc format */
	pr_debug("tc 0x%02x\n", tc);

	if (lowpan_iphc_is_flow_lbl_zero(hdr)) {
		if (!tc) {
			/* 11:  Traffic Class and Flow Label are elided. */
			val = LOWPAN_IPHC_TF_11;
		} else {
			/* 10:  ECN + DSCP (1 byte), Flow Label is elided.
			 *
			 *  0 1 2 3 4 5 6 7
			 * +-+-+-+-+-+-+-+-+
			 * |ECN|   DSCP    |
			 * +-+-+-+-+-+-+-+-+
			 */
			lowpan_push_hc_data(hc_ptr, &tc, sizeof(tc));
			val = LOWPAN_IPHC_TF_10;
		}
	} else {
		/* check if dscp is zero, it's after the first two bit */
		if (!(tc & 0x3f)) {
			/* 01:  ECN + 2-bit Pad + Flow Label (3 bytes), DSCP is elided
			 *
			 *                     1                   2
			 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
			 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
			 * |ECN|rsv|             Flow Label                |
			 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
			 */
			memcpy(&tf[0], &hdr->flow_lbl[0], 3);
			/* zero the highest 4-bits, contains DCSP + ECN */
			tf[0] &= ~0xf0;
			/* set ECN */
			tf[0] |= (tc & 0xc0);

			lowpan_push_hc_data(hc_ptr, tf, 3);
			val = LOWPAN_IPHC_TF_01;
		} else {
			/* 00:  ECN + DSCP + 4-bit Pad + Flow Label (4 bytes)
			 *
			 *                      1                   2                   3
			 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
			 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
			 * |ECN|   DSCP    |  rsv  |             Flow Label                |
			 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
			 */
			memcpy(&tf[0], &tc, sizeof(tc));
			/* highest nibble of flow_lbl[0] is part of DSCP + ECN
			 * which will be the 4-bit pad and will be filled with
			 * zeros afterwards.
			 */
			memcpy(&tf[1], &hdr->flow_lbl[0], 3);
			/* zero the 4-bit pad, which is reserved */
			tf[1] &= ~0xf0;

			lowpan_push_hc_data(hc_ptr, tf, 4);
			val = LOWPAN_IPHC_TF_00;
		}
	}

	return val;
}

static u8 lowpan_iphc_mcast_addr_compress(u8 **hc_ptr,
					  const struct in6_addr *ipaddr)
{
	u8 val;

	if (lowpan_is_mcast_addr_compressable8(ipaddr)) {
		pr_debug("compressed to 1 octet\n");
		/* use last byte */
		lowpan_push_hc_data(hc_ptr, &ipaddr->s6_addr[15], 1);
		val = LOWPAN_IPHC_DAM_11;
	} else if (lowpan_is_mcast_addr_compressable32(ipaddr)) {
		pr_debug("compressed to 4 octets\n");
		/* second byte + the last three */
		lowpan_push_hc_data(hc_ptr, &ipaddr->s6_addr[1], 1);
		lowpan_push_hc_data(hc_ptr, &ipaddr->s6_addr[13], 3);
		val = LOWPAN_IPHC_DAM_10;
	} else if (lowpan_is_mcast_addr_compressable48(ipaddr)) {
		pr_debug("compressed to 6 octets\n");
		/* second byte + the last five */
		lowpan_push_hc_data(hc_ptr, &ipaddr->s6_addr[1], 1);
		lowpan_push_hc_data(hc_ptr, &ipaddr->s6_addr[11], 5);
		val = LOWPAN_IPHC_DAM_01;
	} else {
		pr_debug("using full address\n");
		lowpan_push_hc_data(hc_ptr, ipaddr->s6_addr, 16);
		val = LOWPAN_IPHC_DAM_00;
	}

	return val;
}

int lowpan_header_compress(struct sk_buff *skb, const struct net_device *dev,
			   const void *daddr, const void *saddr)
{
	u8 iphc0, iphc1, *hc_ptr;
	struct ipv6hdr *hdr;
	u8 head[LOWPAN_IPHC_MAX_HC_BUF_LEN] = {};
	int ret, addr_type;

	if (skb->protocol != htons(ETH_P_IPV6))
		return -EINVAL;

	hdr = ipv6_hdr(skb);
	hc_ptr = head + 2;

	pr_debug("IPv6 header dump:\n\tversion = %d\n\tlength  = %d\n"
		 "\tnexthdr = 0x%02x\n\thop_lim = %d\n\tdest    = %pI6c\n",
		 hdr->version, ntohs(hdr->payload_len), hdr->nexthdr,
		 hdr->hop_limit, &hdr->daddr);

	raw_dump_table(__func__, "raw skb network header dump",
		       skb_network_header(skb), sizeof(struct ipv6hdr));

	/* As we copy some bit-length fields, in the IPHC encoding bytes,
	 * we sometimes use |=
	 * If the field is 0, and the current bit value in memory is 1,
	 * this does not work. We therefore reset the IPHC encoding here
	 */
	iphc0 = LOWPAN_DISPATCH_IPHC;
	iphc1 = 0;

	/* TODO: context lookup */

	raw_dump_inline(__func__, "saddr", saddr, EUI64_ADDR_LEN);
	raw_dump_inline(__func__, "daddr", daddr, EUI64_ADDR_LEN);

	raw_dump_table(__func__, "sending raw skb network uncompressed packet",
		       skb->data, skb->len);

	/* Traffic Class, Flow Label compression */
	iphc0 |= lowpan_iphc_tf_compress(&hc_ptr, hdr);

	/* NOTE: payload length is always compressed */

	/* Check if we provide the nhc format for nexthdr and compression
	 * functionality. If not nexthdr is handled inline and not compressed.
	 */
	ret = lowpan_nhc_check_compression(skb, hdr, &hc_ptr);
	if (ret == -ENOENT)
		lowpan_push_hc_data(&hc_ptr, &hdr->nexthdr,
				    sizeof(hdr->nexthdr));
	else
		iphc0 |= LOWPAN_IPHC_NH;

	/* Hop limit
	 * if 1:   compress, encoding is 01
	 * if 64:  compress, encoding is 10
	 * if 255: compress, encoding is 11
	 * else do not compress
	 */
	switch (hdr->hop_limit) {
	case 1:
		iphc0 |= LOWPAN_IPHC_HLIM_01;
		break;
	case 64:
		iphc0 |= LOWPAN_IPHC_HLIM_10;
		break;
	case 255:
		iphc0 |= LOWPAN_IPHC_HLIM_11;
		break;
	default:
		lowpan_push_hc_data(&hc_ptr, &hdr->hop_limit,
				    sizeof(hdr->hop_limit));
	}

	addr_type = ipv6_addr_type(&hdr->saddr);
	/* source address compression */
	if (addr_type == IPV6_ADDR_ANY) {
		pr_debug("source address is unspecified, setting SAC\n");
		iphc1 |= LOWPAN_IPHC_SAC;
	} else {
		if (addr_type & IPV6_ADDR_LINKLOCAL) {
			iphc1 |= lowpan_compress_addr_64(&hc_ptr, &hdr->saddr,
							 saddr, true);
			pr_debug("source address unicast link-local %pI6c iphc1 0x%02x\n",
				 &hdr->saddr, iphc1);
		} else {
			pr_debug("send the full source address\n");
			lowpan_push_hc_data(&hc_ptr, hdr->saddr.s6_addr, 16);
		}
	}

	addr_type = ipv6_addr_type(&hdr->daddr);
	/* destination address compression */
	if (addr_type & IPV6_ADDR_MULTICAST) {
		pr_debug("destination address is multicast: ");
		iphc1 |= LOWPAN_IPHC_M;
		iphc1 |= lowpan_iphc_mcast_addr_compress(&hc_ptr, &hdr->daddr);
	} else {
		if (addr_type & IPV6_ADDR_LINKLOCAL) {
			/* TODO: context lookup */
			iphc1 |= lowpan_compress_addr_64(&hc_ptr, &hdr->daddr,
							 daddr, false);
			pr_debug("dest address unicast link-local %pI6c "
				 "iphc1 0x%02x\n", &hdr->daddr, iphc1);
		} else {
			pr_debug("dest address unicast %pI6c\n", &hdr->daddr);
			lowpan_push_hc_data(&hc_ptr, hdr->daddr.s6_addr, 16);
		}
	}

	/* next header compression */
	if (iphc0 & LOWPAN_IPHC_NH) {
		ret = lowpan_nhc_do_compression(skb, hdr, &hc_ptr);
		if (ret < 0)
			return ret;
	}

	head[0] = iphc0;
	head[1] = iphc1;

	skb_pull(skb, sizeof(struct ipv6hdr));
	skb_reset_transport_header(skb);
	memcpy(skb_push(skb, hc_ptr - head), head, hc_ptr - head);
	skb_reset_network_header(skb);

	pr_debug("header len %d skb %u\n", (int)(hc_ptr - head), skb->len);

	raw_dump_table(__func__, "raw skb data dump compressed",
		       skb->data, skb->len);
	return 0;
}
EXPORT_SYMBOL_GPL(lowpan_header_compress);
