/*
 * Copyright (C) 2011 B.A.T.M.A.N. contributors:
 *
 * Linus Lüssing
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

#ifndef _NET_BATMAN_ADV_MULTICAST_FLOW_H_
#define _NET_BATMAN_ADV_MULTICAST_FLOW_H_

#define THR_CNT_WIN_SIZE	10

struct mcast_flow_entry {
	struct hlist_node list;
	uint8_t mcast_addr[ETH_ALEN];
	unsigned int threshold_count_window[THR_CNT_WIN_SIZE];
	int window_index;
	int threshold_count;
	unsigned long update_timeout;
	unsigned long grace_period_timeout;
	unsigned long last_seen;
	struct rcu_head rcu;
	spinlock_t update_lock;
	atomic_t refcount;
};

void flow_entry_free_ref(struct mcast_flow_entry *flow_entry);
int update_flow_entry(struct mcast_flow_entry *entry,
		      struct bat_priv *bat_priv, int inc);
void mcast_flow_table_free(struct bat_priv *bat_priv);

#endif /* _NET_BATMAN_ADV_MULTICAST_FLOW_H_ */
