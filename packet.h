/*
 * Copyright (C) 2007-2011 B.A.T.M.A.N. contributors:
 *
 * Marek Lindner, Simon Wunderlich
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 *
 */

#ifndef _NET_BATMAN_ADV_PACKET_H_
#define _NET_BATMAN_ADV_PACKET_H_

#define ETH_P_BATMAN  0x4305	/* unofficial/not registered Ethertype */

#define BAT_PACKET_OGM   0x01
#define BAT_ICMP         0x02
#define BAT_UNICAST      0x03
#define BAT_BCAST        0x04
#define BAT_VIS          0x05
#define BAT_UNICAST_FRAG 0x06
#define BAT_TT_QUERY     0x07
#define BAT_ROAM_ADV     0x08

/* this file is included by batctl which needs these defines */
#define COMPAT_VERSION 14
#define DIRECTLINK 0x40
#define VIS_SERVER 0x20
#define PRIMARIES_FIRST_HOP 0x10

/* ICMP message types */
#define ECHO_REPLY 0
#define DESTINATION_UNREACHABLE 3
#define ECHO_REQUEST 8
#define TTL_EXCEEDED 11
#define PARAMETER_PROBLEM 12

/* vis defines */
#define VIS_TYPE_SERVER_SYNC		0
#define VIS_TYPE_CLIENT_UPDATE		1

/* fragmentation defines */
#define UNI_FRAG_HEAD 0x01
#define UNI_FRAG_LARGETAIL 0x02

/* TT flags */
#define TT_RESPONSE     0x01
#define TT_REQUEST      0x02
#define TT_FULL_TABLE   0x04

/* Originator message packet */
struct ogm_packet {
	uint8_t  packet_type;
	uint8_t  version;  /* batman version field */
	uint8_t  ttl;
	uint8_t  flags;    /* 0x40: DIRECTLINK flag, 0x20 VIS_SERVER flag... */
	uint32_t seqno;
	uint8_t  orig[6];
	uint8_t  prev_sender[6];
	uint8_t  gw_flags;  /* flags related to gateway class */
	uint8_t  tq;
	uint8_t  tt_num_changes;
	uint8_t  ttvn; /* translation table version number */
	uint16_t tt_crc;
} __packed;

#define BAT_PACKET_OGM_LEN sizeof(struct ogm_packet)

struct icmp_packet {
	uint8_t  packet_type;
	uint8_t  version;  /* batman version field */
	uint8_t  ttl;
	uint8_t  msg_type; /* see ICMP message types above */
	uint8_t  dst[6];
	uint8_t  orig[6];
	uint16_t seqno;
	uint8_t  uid;
	uint8_t  reserved;
} __packed;

#define BAT_RR_LEN 16

/* icmp_packet_rr must start with all fields from imcp_packet
 * as this is assumed by code that handles ICMP packets */
struct icmp_packet_rr {
	uint8_t  packet_type;
	uint8_t  version;  /* batman version field */
	uint8_t  ttl;
	uint8_t  msg_type; /* see ICMP message types above */
	uint8_t  dst[6];
	uint8_t  orig[6];
	uint16_t seqno;
	uint8_t  uid;
	uint8_t  rr_cur;
	uint8_t  rr[BAT_RR_LEN][ETH_ALEN];
} __packed;

struct unicast_packet {
	uint8_t  packet_type;
	uint8_t  version;  /* batman version field */
	uint8_t  ttl;
	uint8_t  ttvn; /* destination translation table version number */
	uint8_t  dest[6];
} __packed;

struct unicast_frag_packet {
	uint8_t  packet_type;
	uint8_t  version;  /* batman version field */
	uint8_t  ttl;
	uint8_t  ttvn; /* destination translation table version number */
	uint8_t  dest[6];
	uint8_t  flags;
	uint8_t  align;
	uint8_t  orig[6];
	uint16_t seqno;
} __packed;

struct bcast_packet {
	uint8_t  packet_type;
	uint8_t  version;  /* batman version field */
	uint8_t  ttl;
	uint8_t  reserved;
	uint32_t seqno;
	uint8_t  orig[6];
} __packed;

struct vis_packet {
	uint8_t  packet_type;
	uint8_t  version;        /* batman version field */
	uint8_t  ttl;		 /* TTL */
	uint8_t  vis_type;	 /* which type of vis-participant sent this? */
	uint32_t seqno;		 /* sequence number */
	uint8_t  entries;	 /* number of entries behind this struct */
	uint8_t  reserved;
	uint8_t  vis_orig[6];	 /* originator that announces its neighbors */
	uint8_t  target_orig[6]; /* who should receive this packet */
	uint8_t  sender_orig[6]; /* who sent or rebroadcasted this packet */
} __packed;

struct tt_query_packet {
	uint8_t  packet_type;
	uint8_t  version;  /* batman version field */
	uint8_t  ttl;
	/* the flag field is a combination of:
	 * - TT_REQUEST or TT_RESPONSE
	 * - TT_FULL_TABLE */
	uint8_t  flags;
	uint8_t  dst[ETH_ALEN];
	uint8_t  src[ETH_ALEN];
	/* the ttvn field is:
	 * if TT_REQUEST: ttvn that triggered the
	 *		  request
	 * if TT_RESPONSE: new ttvn for the src
	 *		   orig_node */
	uint8_t  ttvn;
	/* tt_data field is:
	 * if TT_REQUEST: crc associated with the
	 *		  ttvn
	 * if TT_RESPONSE: table_size */
	uint16_t tt_data;
} __packed;

struct roam_adv_packet {
	uint8_t  packet_type;
	uint8_t  version;
	uint8_t  ttl;
	uint8_t  reserved;
	uint8_t  dst[ETH_ALEN];
	uint8_t  src[ETH_ALEN];
	uint8_t  client[ETH_ALEN];
} __packed;

#endif /* _NET_BATMAN_ADV_PACKET_H_ */
