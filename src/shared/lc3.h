/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation. All rights reserved.
 *
 */

#define data(args...) ((const unsigned char[]) { args })

#define LC3_IOV(args...) \
	{ \
		.iov_base = (void *)data(args), \
		.iov_len = sizeof(data(args)), \
	}

#define LC3_ID			0x06

#define LC3_BASE		0x01

#define LC3_FREQ		(LC3_BASE)
#define LC3_FREQ_8KHZ		BIT(0)
#define LC3_FREQ_11KHZ		BIT(1)
#define LC3_FREQ_16KHZ		BIT(2)
#define LC3_FREQ_22KHZ		BIT(3)
#define LC3_FREQ_24KHZ		BIT(4)
#define LC3_FREQ_32KHZ		BIT(5)
#define LC3_FREQ_44KHZ		BIT(6)
#define LC3_FREQ_48KHZ		BIT(7)
#define LC3_FREQ_ANY		(LC3_FREQ_8KHZ | \
					LC3_FREQ_11KHZ | \
					LC3_FREQ_16KHZ | \
					LC3_FREQ_22KHZ | \
					LC3_FREQ_24KHZ | \
					LC3_FREQ_32KHZ | \
					LC3_FREQ_44KHZ | \
					LC3_FREQ_48KHZ)

#define LC3_DURATION		(LC3_BASE + 1)
#define LC3_DURATION_7_5	BIT(0)
#define LC3_DURATION_10		BIT(1)
#define LC3_DURATION_ANY	(LC3_DURATION_7_5 | LC3_DURATION_10)
#define LC3_DURATION_PREFER_7_5	BIT(4)
#define LC3_DURATION_PREFER_10	BIT(5)


#define LC3_CHAN_COUNT		(LC3_BASE + 2)
#define LC3_CHAN_COUNT_SUPPORT	BIT(0)

#define LC3_FRAME_LEN		(LC3_BASE + 3)

#define LC3_FRAME_COUNT		(LC3_BASE + 4)

#define LC3_CAPABILITIES(_freq, _duration, _chan_count, _len_min, _len_max) \
	LC3_IOV(0x02, LC3_FREQ, _freq, _freq >> 8, \
		0x02, LC3_DURATION, _duration, \
		0x02, LC3_CHAN_COUNT, _chan_count, \
		0x05, LC3_FRAME_LEN, _len_min, _len_min >> 8, \
		_len_max, _len_max >> 8)

#define LC3_CONFIG_BASE		0x01

#define LC3_CONFIG_FREQ		(LC3_CONFIG_BASE)
#define LC3_CONFIG_FREQ_8KHZ	0x01
#define LC3_CONFIG_FREQ_11KHZ	0x02
#define LC3_CONFIG_FREQ_16KHZ	0x03
#define LC3_CONFIG_FREQ_22KHZ	0x04
#define LC3_CONFIG_FREQ_24KHZ	0x05
#define LC3_CONFIG_FREQ_32KHZ	0x06
#define LC3_CONFIG_FREQ_44KHZ	0x07
#define LC3_CONFIG_FREQ_48KHZ	0x08

#define LC3_CONFIG_DURATION	(LC3_CONFIG_BASE + 1)
#define LC3_CONFIG_DURATION_7_5	0x00
#define LC3_CONFIG_DURATION_10	0x01

#define LC3_CONFIG_CHAN_ALLOC	(LC3_CONFIG_BASE + 2)

#define LC3_CONFIG_FRAME_LEN	(LC3_CONFIG_BASE + 3)

#define LC3_CONFIG(_freq, _duration, _len) \
	LC3_IOV(0x02, LC3_CONFIG_FREQ, _freq, \
		0x02, LC3_CONFIG_DURATION, _duration, \
		0x03, LC3_CONFIG_FRAME_LEN, _len, _len >> 8)

#define LC3_CONFIG_8(_duration, _len) \
	LC3_CONFIG(LC3_CONFIG_FREQ_8KHZ, _duration, _len)

#define LC3_CONFIG_11(_duration, _len) \
	LC3_CONFIG(LC3_CONFIG_FREQ_11KHZ, _duration, _len)

#define LC3_CONFIG_16(_duration, _len) \
	LC3_CONFIG(LC3_CONFIG_FREQ_16KHZ, _duration, _len)

#define LC3_CONFIG_22(_duration, _len) \
	LC3_CONFIG(LC3_CONFIG_FREQ_22KHZ, _duration, _len)

#define LC3_CONFIG_24(_duration, _len) \
	LC3_CONFIG(LC3_CONFIG_FREQ_24KHZ, _duration, _len)

#define LC3_CONFIG_32(_duration, _len) \
	LC3_CONFIG(LC3_CONFIG_FREQ_32KHZ, _duration, _len)

#define LC3_CONFIG_44(_duration, _len) \
	LC3_CONFIG(LC3_CONFIG_FREQ_44KHZ, _duration, _len)

#define LC3_CONFIG_48(_duration, _len) \
	LC3_CONFIG(LC3_CONFIG_FREQ_48KHZ, _duration, _len)

#define LC3_CONFIG_8_1 \
	LC3_CONFIG_8(LC3_CONFIG_DURATION_7_5, 26u)

#define LC3_CONFIG_8_2 \
	LC3_CONFIG_8(LC3_CONFIG_DURATION_10, 30u)

#define LC3_CONFIG_16_1 \
	LC3_CONFIG_16(LC3_CONFIG_DURATION_7_5, 30u)

#define LC3_CONFIG_16_2 \
	LC3_CONFIG_16(LC3_CONFIG_DURATION_10, 40u)

#define LC3_CONFIG_24_1 \
	LC3_CONFIG_24(LC3_CONFIG_DURATION_7_5, 45u)

#define LC3_CONFIG_24_2 \
	LC3_CONFIG_24(LC3_CONFIG_DURATION_10, 60u)

#define LC3_CONFIG_32_1 \
	LC3_CONFIG_32(LC3_CONFIG_DURATION_7_5, 60u)

#define LC3_CONFIG_32_2 \
	LC3_CONFIG_32(LC3_CONFIG_DURATION_10, 80u)

#define LC3_CONFIG_44_1 \
	LC3_CONFIG_44(LC3_CONFIG_DURATION_7_5, 98u)

#define LC3_CONFIG_44_2 \
	LC3_CONFIG_44(LC3_CONFIG_DURATION_10, 130u)

#define LC3_CONFIG_48_1 \
	LC3_CONFIG_48(LC3_CONFIG_DURATION_7_5, 75u)

#define LC3_CONFIG_48_2 \
	LC3_CONFIG_48(LC3_CONFIG_DURATION_10, 100u)

#define LC3_CONFIG_48_3 \
	LC3_CONFIG_48(LC3_CONFIG_DURATION_7_5, 90u)

#define LC3_CONFIG_48_4 \
	LC3_CONFIG_48(LC3_CONFIG_DURATION_10, 120u)

#define LC3_CONFIG_48_5 \
	LC3_CONFIG_48(LC3_CONFIG_DURATION_7_5, 117u)

#define LC3_CONFIG_48_6 \
	LC3_CONFIG_48(LC3_CONFIG_DURATION_10, 155u)

#define LC3_QOS_UNFRAMED	0x00
#define LC3_QOS_FRAMED		0x01

#define LC3_QOS_UCAST(_frame, _pd, _t_lat, _interval, _lat, _sdu, _rtn) \
{ \
	.ucast.cig_id = 0x00, \
	.ucast.cis_id = 0x00, \
	.ucast.delay = _pd, \
	.ucast.target_latency = _t_lat, \
	.ucast.io_qos.interval = _interval, \
	.ucast.io_qos.latency = _lat, \
	.ucast.io_qos.sdu = _sdu, \
	.ucast.io_qos.phy = BT_BAP_CONFIG_PHY_2M, \
	.ucast.io_qos.rtn = _rtn, \
}

#define LC3_QOS_UCAST_7_5_UNFRAMED(_pd, _t_lat, _lat, _sdu, _rtn) \
	LC3_QOS_UCAST(LC3_QOS_UNFRAMED, _pd, _t_lat, 7500u, _lat, _sdu, _rtn)

#define LC3_QOS_UCAST_10_UNFRAMED(_pd, _t_lat, _lat, _sdu, _rtn) \
	LC3_QOS_UCAST(LC3_QOS_UNFRAMED, _pd, _t_lat, 10000u, _lat, _sdu, _rtn)

#define LC3_QOS_UCAST_FRAMED(_pd, _t_lat, _interval, _lat, _sdu, _rtn) \
	LC3_QOS_UCAST(LC3_QOS_FRAMED, _pd, _t_lat, _interval, _lat, _sdu, _rtn)

#define LC3_QOS_8_1_1 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					8u, 26u, 2u)

#define LC3_QOS_8_1_2 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					75u, 26u, 13u)

#define LC3_QOS_8_2_1 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					10u, 30u, 2u)

#define LC3_QOS_8_2_2 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					95u, 30u, 13u)

#define LC3_QOS_16_1_1 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					8u, 30u, 2u)

#define LC3_QOS_16_1_2 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					75u, 30u, 13u)

#define LC3_QOS_16_2_1 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					10u, 40u, 2u)

#define LC3_QOS_16_2_2 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					95u, 40u, 13u)

#define LC3_QOS_24_1_1 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					8u, 45u, 2u)

#define LC3_QOS_24_1_2 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					75u, 45u, 13u)

#define LC3_QOS_24_2_1 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					10u, 60u, 2u)

#define LC3_QOS_24_2_2 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					95u, 60u, 13u)

#define LC3_QOS_32_1_1 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					8u, 60u, 2u)

#define LC3_QOS_32_1_2 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					75u, 60u, 13u)

#define LC3_QOS_32_2_1 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					10u, 80u, 2u)

#define LC3_QOS_32_2_2 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					95u, 80u, 13u)

#define LC3_QOS_44_1_1 \
	LC3_QOS_UCAST_FRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					8163u, 24u, 98u, 5u)

#define LC3_QOS_44_1_2 \
	LC3_QOS_UCAST_FRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					8163u, 80u, 98u, 13u)

#define LC3_QOS_44_2_1 \
	LC3_QOS_UCAST_FRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					10884u, 31u, 130u, 5u)

#define LC3_QOS_44_2_2 \
	LC3_QOS_UCAST_FRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					10884u, 85u, 130u, 13u)

#define LC3_QOS_48_1_1 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					15u, 75u, 5u)

#define LC3_QOS_48_1_2 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					75u, 75u, 13u)

#define LC3_QOS_48_2_1 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					20u, 100u, 5u)

#define LC3_QOS_48_2_2 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					95u, 100u, 13u)

#define LC3_QOS_48_3_1 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					15u, 90u, 5u)

#define LC3_QOS_48_3_2 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					75u, 90u, 13u)

#define LC3_QOS_48_4_1 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					20u, 120u, 5u)

#define LC3_QOS_48_4_2 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					100u, 120u, 13u)

#define LC3_QOS_48_5_1 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					15u, 117u, 5u)

#define LC3_QOS_48_5_2 \
	LC3_QOS_UCAST_7_5_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					75u, 117u, 13u)

#define LC3_QOS_48_6_1 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					20u, 155u, 5u)

#define LC3_QOS_48_6_2 \
	LC3_QOS_UCAST_10_UNFRAMED(40000u, BT_BAP_CONFIG_LATENCY_BALANCED, \
					100u, 155u, 13u)
