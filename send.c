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

#include "main.h"
#include "send.h"
#include "routing.h"
#include "hash.h"
#include "translation-table.h"
#include "soft-interface.h"
#include "hard-interface.h"
#include "vis.h"
#include "aggregation.h"
#include "gateway_common.h"
#include "originator.h"

static void send_outstanding_bcast_packet(struct work_struct *work);

/* apply hop penalty for a normal link */
static uint8_t hop_penalty(const uint8_t tq, struct bat_priv *bat_priv)
{
	int hop_penalty = atomic_read(&bat_priv->hop_penalty);
	return (tq * (TQ_MAX_VALUE - hop_penalty)) / (TQ_MAX_VALUE);
}

/* when do we schedule our own packet to be sent */
static unsigned long own_send_time(struct bat_priv *bat_priv)
{
	return jiffies + msecs_to_jiffies(
		   atomic_read(&bat_priv->orig_interval) -
		   JITTER + (random32() % 2*JITTER));
}

/* when do we schedule a forwarded packet to be sent */
static unsigned long forward_send_time(void)
{
	return jiffies + msecs_to_jiffies(random32() % (JITTER/2));
}

static int recv_icmp_ttl_exceeded(struct bat_priv *bat_priv,
				  struct sk_buff *skb)
{
	struct orig_node *orig_node = NULL;
	struct neigh_node *neigh_node = NULL;
	struct icmp_packet *icmp_packet;
	int ret = NET_RX_DROP;

	icmp_packet = (struct icmp_packet *)skb->data;

	/* send TTL exceeded if packet is an echo request (traceroute) */
	if (icmp_packet->msg_type != ECHO_REQUEST) {
		pr_debug("Warning - can't forward icmp packet from %pM to "
			 "%pM: ttl exceeded\n", icmp_packet->orig,
			 icmp_packet->dst);
		goto out;
	}

	if (!bat_priv->primary_if)
		goto out;

	/* get routing information */
	rcu_read_lock();
	orig_node = ((struct orig_node *)
		     hash_find(bat_priv->orig_hash, compare_orig, choose_orig,
			       icmp_packet->orig));
	if (!orig_node)
		goto unlock;

	kref_get(&orig_node->refcount);
	neigh_node = orig_node->router;

	if (!neigh_node)
		goto unlock;

	if (!atomic_inc_not_zero(&neigh_node->refcount)) {
		neigh_node = NULL;
		goto unlock;
	}

	rcu_read_unlock();

	/* create a copy of the skb, if needed, to modify it. */
	if (skb_cow(skb, sizeof(struct ethhdr)) < 0)
		goto out;

	icmp_packet = (struct icmp_packet *)skb->data;

	memcpy(icmp_packet->dst, icmp_packet->orig, ETH_ALEN);
	memcpy(icmp_packet->orig,
		bat_priv->primary_if->net_dev->dev_addr, ETH_ALEN);
	icmp_packet->msg_type = TTL_EXCEEDED;
	icmp_packet->header.ttl = TTL;

	send_skb_packet(skb, neigh_node->if_incoming, neigh_node->addr);
	ret = NET_RX_SUCCESS;
	goto out;

unlock:
	rcu_read_unlock();
out:
	if (neigh_node)
		neigh_node_free_ref(neigh_node);
	if (orig_node)
		kref_put(&orig_node->refcount, orig_node_free_ref);
	return ret;
}

static void print_ttl_exceeded(struct sk_buff *skb, int packet_type,
			       struct bat_priv *bat_priv) {
	switch (packet_type) {
	case BAT_PACKET:
		bat_dbg(DBG_BATMAN, bat_priv, "ttl exceeded\n");
		break;
	case BAT_ICMP:
		recv_icmp_ttl_exceeded(bat_priv, skb);
		break;
	case BAT_VIS:
		pr_debug("Error - can't send vis packet: ttl exceeded\n");
		break;
	case BAT_UNICAST:
		pr_debug("Warning - can't forward unicast packet from %pM to "
			 "%pM: ttl exceeded\n",
			 ((struct ethhdr *)skb->data)->h_source,
			 ((struct unicast_packet *)skb->data)->dest);
		break;
	default:
		break;
	}
}

/* send out an already prepared packet to the given address via the
 * specified batman interface; drops it if the current TTL is 1 or less
 * or else reduces the TTL by one */
int send_skb_packet(struct sk_buff *skb,
				struct batman_if *batman_if,
				uint8_t *dst_addr)
{
	struct ethhdr *ethhdr;
	struct batman_header *batman_header;
	struct bat_priv *bat_priv = netdev_priv(batman_if->soft_iface);

	if (batman_if->if_status != IF_ACTIVE)
		goto send_skb_err;

	if (unlikely(!batman_if->net_dev))
		goto send_skb_err;

	if (!(batman_if->net_dev->flags & IFF_UP)) {
		pr_warning("Interface %s is not up - can't send packet via "
			   "that interface!\n", batman_if->net_dev->name);
		goto send_skb_err;
	}

	if (unlikely(!pskb_may_pull(skb, sizeof(struct batman_packet))))
		goto send_skb_err;
	batman_header = (struct batman_header *) skb->data;
	if (batman_header->ttl <= 1) {
		print_ttl_exceeded(skb, batman_header->packet_type, bat_priv);
		goto send_skb_err;
	}
	batman_header->ttl--;

	/* push to the ethernet header. */
	if (my_skb_head_push(skb, sizeof(struct ethhdr)) < 0)
		goto send_skb_err;

	skb_reset_mac_header(skb);

	ethhdr = (struct ethhdr *) skb_mac_header(skb);
	memcpy(ethhdr->h_source, batman_if->net_dev->dev_addr, ETH_ALEN);
	memcpy(ethhdr->h_dest, dst_addr, ETH_ALEN);
	ethhdr->h_proto = __constant_htons(ETH_P_BATMAN);

	skb_set_network_header(skb, ETH_HLEN);
	skb->priority = TC_PRIO_CONTROL;
	skb->protocol = __constant_htons(ETH_P_BATMAN);

	skb->dev = batman_if->net_dev;

	/* dev_queue_xmit() returns a negative result on error.	 However on
	 * congestion and traffic shaping, it drops and returns NET_XMIT_DROP
	 * (which is > 0). This will not be treated as an error. */

	return dev_queue_xmit(skb);
send_skb_err:
	kfree_skb(skb);
	return NET_XMIT_DROP;
}

/* Send a packet to a given interface */
static void send_packet_to_if(struct forw_packet *forw_packet,
			      struct batman_if *batman_if)
{
	struct bat_priv *bat_priv = netdev_priv(batman_if->soft_iface);
	char *fwd_str;
	uint8_t packet_num;
	int16_t buff_pos;
	struct batman_packet *batman_packet;
	struct sk_buff *skb;

	if (batman_if->if_status != IF_ACTIVE)
		return;

	packet_num = 0;
	buff_pos = 0;
	batman_packet = (struct batman_packet *)forw_packet->skb->data;

	/* adjust all flags and log packets */
	while (aggregated_packet(buff_pos,
				 forw_packet->packet_len,
				 batman_packet->num_hna)) {

		/* we might have aggregated direct link packets with an
		 * ordinary base packet */
		if ((forw_packet->direct_link_flags & (1 << packet_num)) &&
		    (forw_packet->if_incoming == batman_if))
			batman_packet->flags |= DIRECTLINK;
		else
			batman_packet->flags &= ~DIRECTLINK;

		fwd_str = (packet_num > 0 ? "Forwarding" : (forw_packet->own ?
							    "Sending own" :
							    "Forwarding"));
		bat_dbg(DBG_BATMAN, bat_priv,
			"%s %spacket (originator %pM, seqno %d, TQ %d, TTL %d,"
			" IDF %s) on interface %s [%pM]\n",
			fwd_str, (packet_num > 0 ? "aggregated " : ""),
			batman_packet->orig, ntohl(batman_packet->seqno),
			batman_packet->tq, batman_packet->header.ttl,
			(batman_packet->flags & DIRECTLINK ?
			 "on" : "off"),
			batman_if->net_dev->name, batman_if->net_dev->dev_addr);

		buff_pos += sizeof(struct batman_packet) +
			(batman_packet->num_hna * ETH_ALEN);
		packet_num++;
		batman_packet = (struct batman_packet *)
			(forw_packet->skb->data + buff_pos);
	}

	/* create clone because function is called more than once */
	skb = skb_clone(forw_packet->skb, GFP_ATOMIC);
	if (skb)
		send_skb_packet(skb, batman_if, broadcast_addr);
}

/* send a batman packet */
static void send_packet(struct forw_packet *forw_packet)
{
	struct batman_if *batman_if;
	struct net_device *soft_iface;
	struct bat_priv *bat_priv;
	struct batman_packet *batman_packet =
		(struct batman_packet *)(forw_packet->skb->data);
	unsigned char directlink = (batman_packet->flags & DIRECTLINK ? 1 : 0);

	if (!forw_packet->if_incoming) {
		pr_err("Error - can't forward packet: incoming iface not "
		       "specified\n");
		return;
	}

	soft_iface = forw_packet->if_incoming->soft_iface;
	bat_priv = netdev_priv(soft_iface);

	if (forw_packet->if_incoming->if_status != IF_ACTIVE)
		return;

	/* multihomed peer assumed */
	/* non-primary OGMs are only broadcasted on their interface */
	if ((directlink && (batman_packet->header.ttl == 2)) ||
	    (forw_packet->own && (forw_packet->if_incoming->if_num > 0))) {

		/* FIXME: what about aggregated packets ? */
		bat_dbg(DBG_BATMAN, bat_priv,
			"%s packet (originator %pM, seqno %d, TTL %d) "
			"on interface %s [%pM]\n",
			(forw_packet->own ? "Sending own" : "Forwarding"),
			batman_packet->orig, ntohl(batman_packet->seqno),
			batman_packet->header.ttl,
			forw_packet->if_incoming->net_dev->name,
			forw_packet->if_incoming->net_dev->dev_addr);

		/* skb is only used once and than forw_packet is free'd */
		send_skb_packet(forw_packet->skb, forw_packet->if_incoming,
				broadcast_addr);
		forw_packet->skb = NULL;

		return;
	}

	/* broadcast on every interface */
	rcu_read_lock();
	list_for_each_entry_rcu(batman_if, &if_list, list) {
		if (batman_if->soft_iface != soft_iface)
			continue;

		send_packet_to_if(forw_packet, batman_if);
	}
	rcu_read_unlock();
}

static void rebuild_batman_packet(struct bat_priv *bat_priv,
				  struct batman_if *batman_if)
{
	int new_len;
	unsigned char *new_buff;
	struct batman_packet *batman_packet;

	new_len = sizeof(struct batman_packet) +
			(bat_priv->num_local_hna * ETH_ALEN);
	new_buff = kmalloc(new_len, GFP_ATOMIC);

	/* keep old buffer if kmalloc should fail */
	if (new_buff) {
		memcpy(new_buff, batman_if->packet_buff,
		       sizeof(struct batman_packet));
		batman_packet = (struct batman_packet *)new_buff;

		batman_packet->num_hna = hna_local_fill_buffer(bat_priv,
				new_buff + sizeof(struct batman_packet),
				new_len - sizeof(struct batman_packet));

		kfree(batman_if->packet_buff);
		batman_if->packet_buff = new_buff;
		batman_if->packet_len = new_len;
	}
}

void schedule_own_packet(struct batman_if *batman_if)
{
	struct bat_priv *bat_priv = netdev_priv(batman_if->soft_iface);
	unsigned long send_time;
	struct batman_packet *batman_packet;
	int vis_server;

	if ((batman_if->if_status == IF_NOT_IN_USE) ||
	    (batman_if->if_status == IF_TO_BE_REMOVED))
		return;

	vis_server = atomic_read(&bat_priv->vis_mode);

	/**
	 * the interface gets activated here to avoid race conditions between
	 * the moment of activating the interface in
	 * hardif_activate_interface() where the originator mac is set and
	 * outdated packets (especially uninitialized mac addresses) in the
	 * packet queue
	 */
	if (batman_if->if_status == IF_TO_BE_ACTIVATED)
		batman_if->if_status = IF_ACTIVE;

	/* if local hna has changed and interface is a primary interface */
	if ((atomic_read(&bat_priv->hna_local_changed)) &&
	    (batman_if == bat_priv->primary_if))
		rebuild_batman_packet(bat_priv, batman_if);

	/**
	 * NOTE: packet_buff might just have been re-allocated in
	 * rebuild_batman_packet()
	 */
	batman_packet = (struct batman_packet *)batman_if->packet_buff;

	/* change sequence number to network order */
	batman_packet->seqno =
		htonl((uint32_t)atomic_read(&batman_if->seqno));

	if (vis_server == VIS_TYPE_SERVER_SYNC)
		batman_packet->flags |= VIS_SERVER;
	else
		batman_packet->flags &= ~VIS_SERVER;

	if ((batman_if == bat_priv->primary_if) &&
	    (atomic_read(&bat_priv->gw_mode) == GW_MODE_SERVER))
		batman_packet->gw_flags =
				(uint8_t)atomic_read(&bat_priv->gw_bandwidth);
	else
		batman_packet->gw_flags = 0;

	atomic_inc(&batman_if->seqno);

	slide_own_bcast_window(batman_if);
	send_time = own_send_time(bat_priv);
	add_bat_packet_to_list(bat_priv,
			       batman_if->packet_buff,
			       batman_if->packet_len,
			       batman_if, 1, send_time);
}

void schedule_forward_packet(struct orig_node *orig_node,
			     struct ethhdr *ethhdr,
			     struct batman_packet *batman_packet,
			     uint8_t directlink, int hna_buff_len,
			     struct batman_if *if_incoming)
{
	struct bat_priv *bat_priv = netdev_priv(if_incoming->soft_iface);
	unsigned char in_tq, in_ttl, tq_avg = 0;
	unsigned long send_time;

	in_tq = batman_packet->tq;
	in_ttl = batman_packet->header.ttl;

	memcpy(batman_packet->prev_sender, ethhdr->h_source, ETH_ALEN);

	/* rebroadcast tq of our best ranking neighbor to ensure the rebroadcast
	 * of our best tq value */
	if ((orig_node->router) && (orig_node->router->tq_avg != 0)) {

		/* rebroadcast ogm of best ranking neighbor as is */
		if (!compare_orig(orig_node->router->addr, ethhdr->h_source)) {
			batman_packet->tq = orig_node->router->tq_avg;

			if (orig_node->router->last_ttl)
				batman_packet->header.ttl =
						orig_node->router->last_ttl;
		}

		tq_avg = orig_node->router->tq_avg;
	}

	/* apply hop penalty */
	batman_packet->tq = hop_penalty(batman_packet->tq, bat_priv);

	bat_dbg(DBG_BATMAN, bat_priv,
		"Forwarding packet: tq_orig: %i, tq_avg: %i, "
		"tq_forw: %i, ttl_orig: %i, ttl_forw: %i\n",
		in_tq, tq_avg, batman_packet->tq, in_ttl,
		batman_packet->header.ttl - 1);

	batman_packet->seqno = htonl(batman_packet->seqno);

	/* switch of primaries first hop flag when forwarding */
	batman_packet->flags &= ~PRIMARIES_FIRST_HOP;
	if (directlink)
		batman_packet->flags |= DIRECTLINK;
	else
		batman_packet->flags &= ~DIRECTLINK;

	send_time = forward_send_time();
	add_bat_packet_to_list(bat_priv,
			       (unsigned char *)batman_packet,
			       sizeof(struct batman_packet) + hna_buff_len,
			       if_incoming, 0, send_time);
}

static void forw_packet_free(struct forw_packet *forw_packet)
{
	if (forw_packet->skb)
		kfree_skb(forw_packet->skb);
	kfree(forw_packet);
}

static void _add_bcast_packet_to_list(struct bat_priv *bat_priv,
				      struct forw_packet *forw_packet,
				      unsigned long send_time)
{
	INIT_HLIST_NODE(&forw_packet->list);

	/* add new packet to packet list */
	spin_lock_bh(&bat_priv->forw_bcast_list_lock);
	hlist_add_head(&forw_packet->list, &bat_priv->forw_bcast_list);
	spin_unlock_bh(&bat_priv->forw_bcast_list_lock);

	/* start timer for this packet */
	INIT_DELAYED_WORK(&forw_packet->delayed_work,
			  send_outstanding_bcast_packet);
	queue_delayed_work(bat_event_workqueue, &forw_packet->delayed_work,
			   send_time);
}

#define atomic_dec_not_zero(v)          atomic_add_unless((v), -1, 0)
/* add a broadcast packet to the queue and setup timers. broadcast packets
 * are sent multiple times to increase probability for beeing received.
 *
 * This function returns NETDEV_TX_OK on success and NETDEV_TX_BUSY on
 * errors.
 *
 * The skb is not consumed, so the caller should make sure that the
 * skb is freed. */
int add_bcast_packet_to_list(struct bat_priv *bat_priv, struct sk_buff *skb)
{
	struct forw_packet *forw_packet;

	if (!atomic_dec_not_zero(&bat_priv->bcast_queue_left)) {
		bat_dbg(DBG_BATMAN, bat_priv, "bcast packet queue full\n");
		goto out;
	}

	if (!bat_priv->primary_if)
		goto out;

	forw_packet = kmalloc(sizeof(struct forw_packet), GFP_ATOMIC);

	if (!forw_packet)
		goto out_and_inc;

	skb = skb_copy(skb, GFP_ATOMIC);
	if (!skb)
		goto packet_free;

	skb_reset_mac_header(skb);

	forw_packet->skb = skb;
	forw_packet->if_incoming = bat_priv->primary_if;

	/* how often did we send the bcast packet ? */
	forw_packet->num_packets = 0;

	_add_bcast_packet_to_list(bat_priv, forw_packet, 1);
	return NETDEV_TX_OK;

packet_free:
	kfree(forw_packet);
out_and_inc:
	atomic_inc(&bat_priv->bcast_queue_left);
out:
	return NETDEV_TX_BUSY;
}

static void send_outstanding_bcast_packet(struct work_struct *work)
{
	struct batman_if *batman_if;
	struct delayed_work *delayed_work =
		container_of(work, struct delayed_work, work);
	struct forw_packet *forw_packet =
		container_of(delayed_work, struct forw_packet, delayed_work);
	struct sk_buff *skb1;
	struct net_device *soft_iface = forw_packet->if_incoming->soft_iface;
	struct bat_priv *bat_priv = netdev_priv(soft_iface);

	spin_lock_bh(&bat_priv->forw_bcast_list_lock);
	hlist_del(&forw_packet->list);
	spin_unlock_bh(&bat_priv->forw_bcast_list_lock);

	if (atomic_read(&bat_priv->mesh_state) == MESH_DEACTIVATING)
		goto out;

	/* rebroadcast packet */
	rcu_read_lock();
	list_for_each_entry_rcu(batman_if, &if_list, list) {
		if (batman_if->soft_iface != soft_iface)
			continue;

		/* send a copy of the saved skb */
		skb1 = skb_clone(forw_packet->skb, GFP_ATOMIC);
		if (skb1)
			send_skb_packet(skb1, batman_if, broadcast_addr);
	}
	rcu_read_unlock();

	forw_packet->num_packets++;

	/* if we still have some more bcasts to send */
	if (forw_packet->num_packets < 3) {
		_add_bcast_packet_to_list(bat_priv, forw_packet,
					  ((5 * HZ) / 1000));
		return;
	}

out:
	forw_packet_free(forw_packet);
	atomic_inc(&bat_priv->bcast_queue_left);
}

void send_outstanding_bat_packet(struct work_struct *work)
{
	struct delayed_work *delayed_work =
		container_of(work, struct delayed_work, work);
	struct forw_packet *forw_packet =
		container_of(delayed_work, struct forw_packet, delayed_work);
	struct bat_priv *bat_priv;

	bat_priv = netdev_priv(forw_packet->if_incoming->soft_iface);
	spin_lock_bh(&bat_priv->forw_bat_list_lock);
	hlist_del(&forw_packet->list);
	spin_unlock_bh(&bat_priv->forw_bat_list_lock);

	if (atomic_read(&bat_priv->mesh_state) == MESH_DEACTIVATING)
		goto out;

	send_packet(forw_packet);

	/**
	 * we have to have at least one packet in the queue
	 * to determine the queues wake up time unless we are
	 * shutting down
	 */
	if (forw_packet->own)
		schedule_own_packet(forw_packet->if_incoming);

out:
	/* don't count own packet */
	if (!forw_packet->own)
		atomic_inc(&bat_priv->batman_queue_left);

	forw_packet_free(forw_packet);
}

void purge_outstanding_packets(struct bat_priv *bat_priv,
			       struct batman_if *batman_if)
{
	struct forw_packet *forw_packet;
	struct hlist_node *tmp_node, *safe_tmp_node;

	if (batman_if)
		bat_dbg(DBG_BATMAN, bat_priv,
			"purge_outstanding_packets(): %s\n",
			batman_if->net_dev->name);
	else
		bat_dbg(DBG_BATMAN, bat_priv,
			"purge_outstanding_packets()\n");

	/* free bcast list */
	spin_lock_bh(&bat_priv->forw_bcast_list_lock);
	hlist_for_each_entry_safe(forw_packet, tmp_node, safe_tmp_node,
				  &bat_priv->forw_bcast_list, list) {

		/**
		 * if purge_outstanding_packets() was called with an argmument
		 * we delete only packets belonging to the given interface
		 */
		if ((batman_if) &&
		    (forw_packet->if_incoming != batman_if))
			continue;

		spin_unlock_bh(&bat_priv->forw_bcast_list_lock);

		/**
		 * send_outstanding_bcast_packet() will lock the list to
		 * delete the item from the list
		 */
		cancel_delayed_work_sync(&forw_packet->delayed_work);
		spin_lock_bh(&bat_priv->forw_bcast_list_lock);
	}
	spin_unlock_bh(&bat_priv->forw_bcast_list_lock);

	/* free batman packet list */
	spin_lock_bh(&bat_priv->forw_bat_list_lock);
	hlist_for_each_entry_safe(forw_packet, tmp_node, safe_tmp_node,
				  &bat_priv->forw_bat_list, list) {

		/**
		 * if purge_outstanding_packets() was called with an argmument
		 * we delete only packets belonging to the given interface
		 */
		if ((batman_if) &&
		    (forw_packet->if_incoming != batman_if))
			continue;

		spin_unlock_bh(&bat_priv->forw_bat_list_lock);

		/**
		 * send_outstanding_bat_packet() will lock the list to
		 * delete the item from the list
		 */
		cancel_delayed_work_sync(&forw_packet->delayed_work);
		spin_lock_bh(&bat_priv->forw_bat_list_lock);
	}
	spin_unlock_bh(&bat_priv->forw_bat_list_lock);
}
