/* Copyright (C) 2012-2016 B.A.T.M.A.N. contributors:
 *
 * Edo Monticelli, Antonio Quartulli
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "tp_meter.h"
#include "main.h"

#include <asm/processor.h>
#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/byteorder/generic.h>
#include <linux/compiler.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/if_ether.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/param.h>
#include <linux/printk.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "hard-interface.h"
#include "icmp_socket.h"
#include "originator.h"
#include "send.h"
#include "packet.h"

/**
 * BATADV_TP_DEF_TEST_LENGTH - Default test length if not specified by the user
 *  in milliseconds
 */
#define BATADV_TP_DEF_TEST_LENGTH 10000

/**
 * BATADV_TP_AWND - Advertised window by the receiver (in bytes)
 */
#define BATADV_TP_AWND 0x20000000

/**
 * BATADV_TP_RECV_TIMEOUT - Receiver activity timeout. If the receiver does not
 *  get anything for such amount of milliseconds, the connection is killed
 */
#define BATADV_TP_RECV_TIMEOUT 1000

/**
 * BATADV_TP_MAX_RTO - Maximum sender timeout. If the sender RTO gets beyond
 * such amound of milliseconds, the receiver is considered unreachable and the
 * connection is killed
 */
#define BATADV_TP_MAX_RTO 30000
/**
 * BATADV_TP_FIRST_SEQ - First seqno of each session. The number is rather high
 *  in order to immediately trigger a wrap around (test purposes)
 */
#define BATADV_TP_FIRST_SEQ ((u32)-1 - 2000)

#define PACKET_LEN 1450

/**
 * batadv_tp_cwnd - compute the new cwnd size
 * @base: base cwnd size value
 * @increment: the value to add to base to get the new size
 * @min: minumim cwnd value (usually MSS)
 *
 * Return the new cwnd size and ensures it does not exceed the Advertised
 * Receiver Window size. It is wrap around safe.
 * For details refer to Section 3.1 of RFC5681
 *
 * Return: TODO
 */
static u32 batadv_tp_cwnd(u32 base, u32 increment, u32 min)
{
	u32 new_size = base + increment;

	/* check for wrap-around */
	if (new_size < base)
		new_size = (u32)ULONG_MAX;

	new_size = min_t(u32, new_size, BATADV_TP_AWND);

	return max_t(u32, new_size, min);
}

/**
 * batadv_tp_batctl_notify - TODO
 * @status: TODO
 * @uid: TODO
 * @start_time: TODO
 * @total_sent: TODO
 */
static void batadv_tp_batctl_notify(u8 status, u8 uid,
				    unsigned long start_time,
				    u32 total_sent)
{
	struct batadv_icmp_tp_result_packet result;

	result.uid = uid;

	if (!batadv_tp_is_error(status)) {
		result.return_value = BATADV_TP_COMPLETE;
		result.test_time = jiffies_to_msecs(jiffies - start_time);
		result.total_bytes = total_sent;
	} else {
		result.return_value = status;
	}

	batadv_socket_receive_packet(&result, sizeof(result));
}

/**
 * batadv_tp_batctl_error_notify - TODO
 * @status: TODO
 * @uid: TODO
 */
static void batadv_tp_batctl_error_notify(u8 status, u8 uid)
{
	batadv_tp_batctl_notify(status, uid, 0, 0);
}

/**
 * batadv_tp_vars_release - release batadv_tp_vars from lists and queue for
 *  free after rcu grace period
 * @ref: kref pointer of the batadv_tp_vars
 */
static void batadv_tp_vars_release(struct kref *ref)
{
	struct batadv_tp_vars *tp_vars;
	struct batadv_tp_unacked *un, *safe;

	tp_vars = container_of(ref, struct batadv_tp_vars, refcount);

	/* lock should not be needed because this object is now out of any
	 * context!
	 */
	spin_lock_bh(&tp_vars->unacked_lock);
	list_for_each_entry_safe(un, safe, &tp_vars->unacked_list, list) {
		list_del(&un->list);
		kfree(un);
	}
	spin_unlock_bh(&tp_vars->unacked_lock);

	kfree_rcu(tp_vars, rcu);
}

/**
 * batadv_tp_vars_put - decrement the batadv_tp_vars refcounter and possibly
 *  release it
 * @tp_vars: batadv_tp_vars gateway to be free'd
 */
static void batadv_tp_vars_put(struct batadv_tp_vars *tp_vars)
{
	kref_put(&tp_vars->refcount, batadv_tp_vars_release);
}

/**
 * batadv_tp_update_rto - TODO
 * @tp_vars: TODO
 * @new_rtt: TODO
 */
static void batadv_tp_update_rto(struct batadv_tp_vars *tp_vars,
				 u32 new_rtt)
{
	long m = new_rtt;

	/* RTT update
	 * Details in Section 2.2 and 2.3 of RFC6298
	 *
	 * It's tricky to understand. Don't lose hair please.
	 * Inspired by tcp_rtt_estimator() tcp_input.c
	 */
	if (tp_vars->srtt != 0) {
		m -= (tp_vars->srtt >> 3); /* m is now error in rtt est */
		tp_vars->srtt += m; /* rtt = 7/8 srtt + 1/8 new */
		if (m < 0)
			m = -m;

		m -= (tp_vars->rttvar >> 2);
		tp_vars->rttvar += m; /* mdev ~= 3/4 rttvar + 1/4 new */
	} else {
		/* first measure getting in */
		tp_vars->srtt = m << 3;	/* take the measured time to be srtt */
		tp_vars->rttvar = m << 1; /* new_rtt / 2 */
	}

	/* rto = srtt + 4 * rttvar.
	 * rttvar is scaled by 4, therefore doesn't need to be multiplied
	 */
	tp_vars->rto = (tp_vars->srtt >> 3) + tp_vars->rttvar;
}

/**
 * batadv_tp_cleanup - TODO
 * @bat_priv: TODO
 * @tp_vars: TODO
 */
static void batadv_tp_cleanup(struct batadv_priv *bat_priv,
			      struct batadv_tp_vars *tp_vars)
{
	cancel_delayed_work(&tp_vars->finish_work);

	spin_lock_bh(&tp_vars->bat_priv->tp_list_lock);
	hlist_del_rcu(&tp_vars->list);
	spin_unlock_bh(&tp_vars->bat_priv->tp_list_lock);

	/* drop list reference */
	batadv_tp_vars_put(tp_vars);

	atomic_dec(&tp_vars->bat_priv->tp_num);

	/* kill the timer and remove its reference */
	del_timer_sync(&tp_vars->timer);
	/* the worker might have rearmed itself therefore we kill it again. Note
	 * that if the worker should run again before invoking the following
	 * del_timer(), it would not re-arm itself once again because the status
	 * is OFF now
	 */
	del_timer(&tp_vars->timer);
	batadv_tp_vars_put(tp_vars);
}

/**
 * batadv_tp_finish - TODO
 * @bat_priv: TODO
 * @tp_vars: TODO
 */
static void batadv_tp_finish(struct batadv_priv *bat_priv,
			     struct batadv_tp_vars *tp_vars)
{
	batadv_tp_cleanup(bat_priv, tp_vars);

	batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
		   "Test towards %pM finished..shutting down (reason=%d)\n",
		   tp_vars->other_end, tp_vars->reason);

	batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
		   "Last timing stats: SRTT=%ums RTTVAR=%ums RTO=%ums\n",
		   tp_vars->srtt >> 3, tp_vars->rttvar >> 2, tp_vars->rto);

	batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
		   "Final values: cwnd=%u ss_threshold=%u\n",
		   tp_vars->cwnd, tp_vars->ss_threshold);

	batadv_tp_batctl_notify(tp_vars->reason,
				tp_vars->socket_client->index,
				tp_vars->start_time,
				atomic_read(&tp_vars->tot_sent));
}

/**
 * batadv_tp_shutdown - TODO
 * @tp_vars: TODO
 * @reason: TODO
 */
static void batadv_tp_shutdown(struct batadv_tp_vars *tp_vars, int reason)
{
	if (!atomic_dec_and_test(&tp_vars->sending))
		return;

	tp_vars->reason = reason;
}

/**
 * batadv_tp_reset_receiver_timer - reset the receiver shutdown timer
 * @tp_vars: the private data of the current TP meter session
 *
 * start the receiver shutdown timer or reset it if already started
 */
static void batadv_tp_reset_receiver_timer(struct batadv_tp_vars *tp_vars)
{
	mod_timer(&tp_vars->timer,
		  jiffies + msecs_to_jiffies(BATADV_TP_RECV_TIMEOUT));
}

/**
 * batadv_tp_reset_sender_timer - reschedule the sender timer
 * @tp_vars: the private TP meter data for this session
 *
 * Reschedule the timer using tp_vars->rto as delay
 */
static void batadv_tp_reset_sender_timer(struct batadv_tp_vars *tp_vars)
{
	/* most of the time this function is invoked while normal packet
	 * reception...
	 */
	if (unlikely(atomic_read(&tp_vars->sending) == 0))
		/* timer reference will be dropped in batadv_tp_cleanup */
		return;

	mod_timer(&tp_vars->timer, jiffies + msecs_to_jiffies(tp_vars->rto));
}

/**
 * batadv_tp_list_find - find a tp_vars object in the global list
 * @bat_priv: the bat priv with all the soft interface information
 * @dst: the other endpoint address to look for
 *
 * Look for a tp_vars object matching dst as end_point and return it after
 * having incremented the refcounter. Return NULL is not found
 *
 * Return: TODO
 */
static struct batadv_tp_vars *batadv_tp_list_find(struct batadv_priv *bat_priv,
						  u8 *dst)
{
	struct batadv_tp_vars *pos, *tp_vars = NULL;

	rcu_read_lock();
	hlist_for_each_entry_rcu(pos, &bat_priv->tp_list, list) {
		/* most of the time this function is invoked during the normal
		 * process..it makes sens to pay more when the session is
		 * finished and to speed the process up during the measurement
		 */
		if (unlikely(!kref_get_unless_zero(&pos->refcount)))
			continue;

		if (!batadv_compare_eth(pos->other_end, dst))
			continue;

		tp_vars = pos;
		break;
	}
	rcu_read_unlock();

	return tp_vars;
}

/**
 * batadv_tp_send_ack - send an ACK packet
 * @bat_priv: the bat priv with all the soft interface information
 * @dst: the mac address of the destination originator
 * @seq: the sequence number to ACK
 * @timestamp: the timestamp to echo back in the ACK
 * @socket_index: TODO
 *
 * Return: 0 on success, a positive integer representing the reason of the
 * failure otherwise
 */
static int batadv_tp_send_ack(struct batadv_priv *bat_priv, u8 *dst,
			      u32 seq, __be32 timestamp,
			      int socket_index)
{
	struct batadv_hard_iface *primary_if = NULL;
	struct batadv_orig_node *orig_node;
	struct batadv_icmp_tp_packet *icmp;
	struct sk_buff *skb;
	int r, ret;

	orig_node = batadv_orig_hash_find(bat_priv, dst);
	if (unlikely(!orig_node)) {
		ret = BATADV_TP_DST_UNREACHABLE;
		goto out;
	}

	primary_if = batadv_primary_if_get_selected(bat_priv);
	if (unlikely(!primary_if)) {
		ret = BATADV_TP_DST_UNREACHABLE;
		goto out;
	}

	skb = netdev_alloc_skb_ip_align(NULL, sizeof(*icmp) + ETH_HLEN);
	if (unlikely(!skb)) {
		ret = BATADV_TP_MEMORY_ERROR;
		goto out;
	}

	skb_reserve(skb, ETH_HLEN);
	icmp = (struct batadv_icmp_tp_packet *)skb_put(skb, sizeof(*icmp));
	icmp->packet_type = BATADV_ICMP;
	icmp->version = BATADV_COMPAT_VERSION;
	icmp->ttl = BATADV_TTL;
	icmp->msg_type = BATADV_TP;
	ether_addr_copy(icmp->dst, orig_node->orig);
	ether_addr_copy(icmp->orig, primary_if->net_dev->dev_addr);
	icmp->uid = socket_index;

	icmp->subtype = BATADV_TP_ACK;
	icmp->seqno = htonl(seq);
	icmp->timestamp = timestamp;

	/* send the ack */
	r = batadv_send_skb_to_orig(skb, orig_node, NULL);
	if (unlikely(r < 0) || (r == NET_XMIT_DROP)) {
		ret = BATADV_TP_DST_UNREACHABLE;
		goto out;
	}
	ret = 0;

out:
	if (likely(orig_node))
		batadv_orig_node_put(orig_node);
	if (likely(primary_if))
		batadv_hardif_put(primary_if);

	return ret;
}

/**
 * batadv_tp_handle_out_of_order - store an out of order packet
 * @tp_vars: the private data of the current TP meter session
 * @skb: the buffer containing the received packet
 *
 * Store the out of order packet in the unacked list for late processing. This
 * packets are kept in this list so that they can be ACKed at once as soon as
 * all the previous packets have been received
 *
 * Return: true if the packed has been successfully processed, false otherwise
 */
static bool batadv_tp_handle_out_of_order(struct batadv_tp_vars *tp_vars,
					  struct sk_buff *skb)
{
	struct batadv_icmp_tp_packet *icmp;
	struct batadv_tp_unacked *un, *new;
	u32 payload_len;
	bool added = false;

	new = kmalloc(sizeof(*new), GFP_ATOMIC);
	if (unlikely(!new))
		return false;

	icmp = (struct batadv_icmp_tp_packet *)skb->data;

	new->seqno = ntohl(icmp->seqno);
	payload_len = skb->len - sizeof(struct batadv_unicast_packet);
	new->len = payload_len;

	spin_lock_bh(&tp_vars->unacked_lock);
	/* if the list is empty immediately attach this new object */
	if (list_empty(&tp_vars->unacked_list)) {
		list_add(&new->list, &tp_vars->unacked_list);
		goto out;
	}

	/* otherwise loop over the list and either drop the packet because this
	 * is a duplicate or store it at the right position.
	 *
	 * The iteration is done in the reverse way because it is likely that
	 * the last received packet (the one being processed now) has a bigger
	 * seqno than all the others already stored.
	 */
	list_for_each_entry_reverse(un, &tp_vars->unacked_list, list) {
		/* check for duplicates */
		if (new->seqno == un->seqno) {
			if (new->len > un->len)
				un->len = new->len;
			kfree(new);
			added = true;
			break;
		}

		/* look for the right position */
		if (batadv_seq_before(new->seqno, un->seqno))
			continue;

		/* as soon as an entry having a bigger seqno is found, the new
		 * one is attached _after_ it. In this way the list is kept in
		 * ascending order
		 */
		list_add_tail(&new->list, &un->list);
		added = true;
		break;
	}

	/* received packet with smallest seqno out of order; add it to front */
	if (!added)
		list_add(&new->list, &tp_vars->unacked_list);

out:
	spin_unlock_bh(&tp_vars->unacked_lock);

	return true;
}

/**
 * batadv_tp_receiver_shutdown - TODO
 * @arg: TODO
 */
static void batadv_tp_receiver_shutdown(unsigned long arg)
{
	struct batadv_tp_vars *tp_vars = (struct batadv_tp_vars *)arg;
	struct batadv_tp_unacked *un, *safe;
	struct batadv_priv *bat_priv;

	bat_priv = tp_vars->bat_priv;

	/* if there is recent activity rearm the timer */
	if (!batadv_has_timed_out(tp_vars->last_recv_time,
				  BATADV_TP_RECV_TIMEOUT)) {
		/* reset the receiver shutdown timer */
		batadv_tp_reset_receiver_timer(tp_vars);
		return;
	}

	batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
		   "Shutting down for inactivity (more than %dms) from %pM\n",
		   BATADV_TP_RECV_TIMEOUT, tp_vars->other_end);

	spin_lock_bh(&tp_vars->bat_priv->tp_list_lock);
	hlist_del_rcu(&tp_vars->list);
	spin_unlock_bh(&tp_vars->bat_priv->tp_list_lock);

	/* drop list reference */
	batadv_tp_vars_put(tp_vars);

	atomic_dec(&bat_priv->tp_num);

	spin_lock_bh(&tp_vars->unacked_lock);
	list_for_each_entry_safe(un, safe, &tp_vars->unacked_list, list) {
		list_del(&un->list);
		kfree(un);
	}
	spin_unlock_bh(&tp_vars->unacked_lock);

	/* drop reference of timer */
	batadv_tp_vars_put(tp_vars);
}

/**
 * batadv_tp_init_recv - TODO
 * @bat_priv: TODO
 * @icmp: TODO
 *
 * Return: TODO
 */
static struct batadv_tp_vars *
batadv_tp_init_recv(struct batadv_priv *bat_priv,
		    struct batadv_icmp_tp_packet *icmp)
{
	struct batadv_tp_vars *tp_vars;

	spin_lock_bh(&bat_priv->tp_list_lock);
	tp_vars = batadv_tp_list_find(bat_priv, icmp->orig);
	if (tp_vars)
		goto out_unlock;

	if (!atomic_add_unless(&bat_priv->tp_num, 1, BATADV_TP_MAX_NUM)) {
		batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
			   "Meter: too many ongoing sessions, aborting (RECV)\n");
		goto out_unlock;
	}

	tp_vars = kmalloc(sizeof(*tp_vars), GFP_ATOMIC);
	if (!tp_vars)
		goto out_unlock;

	ether_addr_copy(tp_vars->other_end, icmp->orig);
	tp_vars->status = BATADV_TP_RECEIVER;
	tp_vars->last_recv = BATADV_TP_FIRST_SEQ;
	tp_vars->bat_priv = bat_priv;
	kref_init(&tp_vars->refcount);

	spin_lock_init(&tp_vars->unacked_lock);
	INIT_LIST_HEAD(&tp_vars->unacked_list);

	kref_get(&tp_vars->refcount);
	hlist_add_head_rcu(&tp_vars->list, &bat_priv->tp_list);

	kref_get(&tp_vars->refcount);
	setup_timer(&tp_vars->timer, batadv_tp_receiver_shutdown,
		    (unsigned long)tp_vars);

	batadv_tp_reset_receiver_timer(tp_vars);

out_unlock:
	spin_unlock_bh(&bat_priv->tp_list_lock);

	return tp_vars;
}

/**
 * batadv_tp_ack_unordered - TODO
 * @tp_vars: TODO
 */
static void batadv_tp_ack_unordered(struct batadv_tp_vars *tp_vars)
{
	struct batadv_tp_unacked *un, *safe;
	u32 to_ack;

	/* go through the unacked packet list and possibly ACK them as
	 * well
	 */
	spin_lock_bh(&tp_vars->unacked_lock);
	list_for_each_entry_safe(un, safe, &tp_vars->unacked_list, list) {
		/* the list is ordered, therefore it is possible to stop as soon
		 * there is a gap between the last acked seqno and the seqno of
		 * the packet under inspection
		 */
		if (batadv_seq_before(tp_vars->last_recv, un->seqno))
			break;

		to_ack = un->seqno + un->len - tp_vars->last_recv;

		if (batadv_seq_before(tp_vars->last_recv, un->seqno + un->len))
			tp_vars->last_recv += to_ack;

		list_del(&un->list);
		kfree(un);
	}
	spin_unlock_bh(&tp_vars->unacked_lock);
}

/**
 * batadv_tp_recv_msg - process a single data message
 * @bat_priv: the bat priv with all the soft interface information
 * @skb: the buffer containing the received packet
 *
 * Process a received TP MSG packet
 */
static void batadv_tp_recv_msg(struct batadv_priv *bat_priv,
			       struct sk_buff *skb)
{
	struct batadv_icmp_tp_packet *icmp;
	struct batadv_tp_vars *tp_vars;
	size_t packet_size;
	u32 seqno;

	icmp = (struct batadv_icmp_tp_packet *)skb->data;

	seqno = ntohl(icmp->seqno);
	/* check if this is the first seqno. This means that if the
	 * first packet is lost, the tp meter does not work anymore!
	 */
	if (seqno == BATADV_TP_FIRST_SEQ) {
		tp_vars = batadv_tp_init_recv(bat_priv, icmp);
		if (!tp_vars) {
			batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
				   "Meter: seqno != BATADV_TP_FIRST_SEQ cannot initiate connection\n");
			goto out;
		}
	} else {
		tp_vars = batadv_tp_list_find(bat_priv, icmp->orig);
		if (!tp_vars) {
			batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
				   "Unexpected packet from %pM!\n",
				   icmp->orig);
			goto out;
		}
	}

	if (unlikely(tp_vars->status != BATADV_TP_RECEIVER)) {
		batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
			   "Meter: dropping packet: not expected (status=%u)\n",
			   tp_vars->status);
		goto out;
	}

	tp_vars->last_recv_time = jiffies;

	/* if the packet is a duplicate, it may be the case that an ACK has been
	 * lost. Resend the ACK
	 */
	if (batadv_seq_before(seqno, tp_vars->last_recv))
		goto send_ack;

	/* if the packet is out of order enqueue it */
	if (ntohl(icmp->seqno) != tp_vars->last_recv) {
		/* exit immediately (and do not send any ACK) if the packet has
		 * not been enqueued correctly
		 */
		if (!batadv_tp_handle_out_of_order(tp_vars, skb))
			goto out;

		/* send a duplicate ACK */
		goto send_ack;
	}

	/* if everything was fine count the ACKed bytes */
	packet_size = skb->len - sizeof(struct batadv_unicast_packet);
	tp_vars->last_recv += packet_size;

	/* check if this ordered message filled a gap.... */
	batadv_tp_ack_unordered(tp_vars);

send_ack:
	/* send the ACK. If the received packet was out of order, the ACK that
	 * is going to be sent is a duplicate (the sender will count them and
	 * possibly enter Fast Retransmit as soon as it has reached 3)
	 */
	batadv_tp_send_ack(bat_priv, icmp->orig, tp_vars->last_recv,
			   icmp->timestamp, icmp->uid);
out:
	if (likely(tp_vars))
		batadv_tp_vars_put(tp_vars);
}

/**
 * batadv_tp_send_msg - send a single message
 * @src: source mac address
 * @orig_node: TODO
 * @seqno: sequence number of this packet
 * @len: length of the entire packet
 * @socket_index: TODO
 * @timestamp: TODO
 *
 * Create and send a single TP Meter message.
 *
 * Return: 0 on success, BATADV_TP_DST_UNREACHABLE if the destination is not
 * reachable, BATADV_TP_MEMORY_ERROR if the packet couldn't be allocated
 */
static int batadv_tp_send_msg(u8 *src, struct batadv_orig_node *orig_node,
			      u32 seqno, size_t len, int socket_index,
			      u32 timestamp)
{
	struct batadv_icmp_tp_packet *icmp;
	struct sk_buff *skb;
	int r;

	skb = netdev_alloc_skb_ip_align(NULL, len + ETH_HLEN);
	if (unlikely(!skb))
		return BATADV_TP_MEMORY_ERROR;

	skb_reserve(skb, ETH_HLEN);
	icmp = (struct batadv_icmp_tp_packet *)skb_put(skb, len);

	/* fill the icmp header */
	ether_addr_copy(icmp->dst, orig_node->orig);
	ether_addr_copy(icmp->orig, src);
	icmp->version = BATADV_COMPAT_VERSION;
	icmp->packet_type = BATADV_ICMP;
	icmp->ttl = BATADV_TTL;
	icmp->msg_type = BATADV_TP;
	icmp->uid = socket_index;

	icmp->subtype = BATADV_TP_MSG;
	icmp->seqno = htonl(seqno);
	icmp->timestamp = htonl(timestamp);

	/* TODO WAIT - we are sending uninitialized data? */
	r = batadv_send_skb_to_orig(skb, orig_node, NULL);
	if (r < 0)
		kfree_skb(skb);

	if (r == NET_XMIT_SUCCESS)
		return 0;

	return BATADV_TP_CANT_SEND;
}

/**
 * batadv_tp_sender_finish - TODO
 * @work: TODO
 */
static void batadv_tp_sender_finish(struct work_struct *work)
{
	struct delayed_work *delayed_work;
	struct batadv_tp_vars *tp_vars;

	delayed_work = container_of(work, struct delayed_work, work);
	tp_vars = container_of(delayed_work, struct batadv_tp_vars,
			       finish_work);

	batadv_tp_shutdown(tp_vars, BATADV_TP_COMPLETE);
}

/**
 * batadv_tp_avail - TODO
 * @tp_vars: TODO
 * @payload_len: TODO
 *
 * Return: TODO
 */
static bool batadv_tp_avail(struct batadv_tp_vars *tp_vars,
			    size_t payload_len)
{
	u32 win_left, win_limit;

	win_limit = atomic_read(&tp_vars->last_acked) + tp_vars->cwnd;
	win_left = win_limit - tp_vars->last_sent;

	return win_left >= payload_len;
}

/**
 * batadv_tp_wait_available - TODO
 * @tp_vars: TODO
 * @payload_len: TODO
 */
static void batadv_tp_wait_available(struct batadv_tp_vars *tp_vars,
				     size_t payload_len)
{
	wait_event_interruptible_timeout(tp_vars->more_bytes,
					 batadv_tp_avail(tp_vars,
							 payload_len),
					 HZ / 10);
}

/**
 * batadv_tp_send - TODO
 * @arg: TODO
 *
 * Return: TODO
 */
static int batadv_tp_send(void *arg)
{
	struct batadv_tp_vars *tp_vars = arg;
	struct batadv_priv *bat_priv = tp_vars->bat_priv;
	struct batadv_hard_iface *primary_if = NULL;
	struct batadv_orig_node *orig_node = NULL;
	size_t payload_len, packet_len;
	int err = 0;

	if (unlikely(tp_vars->status != BATADV_TP_SENDER)) {
		err = BATADV_TP_DST_UNREACHABLE;
		goto out;
	}

	orig_node = batadv_orig_hash_find(bat_priv, tp_vars->other_end);
	if (unlikely(!orig_node)) {
		err = BATADV_TP_DST_UNREACHABLE;
		goto out;
	}

	primary_if = batadv_primary_if_get_selected(bat_priv);
	if (unlikely(!primary_if)) {
		err = BATADV_TP_DST_UNREACHABLE;
		goto out;
	}

	/* assume that all the hard_interfaces have a correctly
	 * configured MTU, so use the soft_iface MTU as MSS.
	 * This might not be true and in that case the fragmentation
	 * should be used.
	 * Now, try to send the packet as it is
	 */
	payload_len = PACKET_LEN;
	BUILD_BUG_ON(sizeof(struct batadv_icmp_tp_packet) > PACKET_LEN);

	batadv_tp_reset_sender_timer(tp_vars);

	/* queue the worker in charge of terminating the test */
	queue_delayed_work(batadv_event_workqueue, &tp_vars->finish_work,
			   msecs_to_jiffies(tp_vars->test_length));

	while (atomic_read(&tp_vars->sending) != 0) {
		if (unlikely(!batadv_tp_avail(tp_vars, payload_len))) {
			batadv_tp_wait_available(tp_vars, payload_len);
			continue;
		}

		/* to emulate normal unicast traffic, add to the payload len
		 * the size of the unicast header
		 */
		packet_len = payload_len + sizeof(struct batadv_unicast_packet);

		err = batadv_tp_send_msg(primary_if->net_dev->dev_addr,
					 orig_node, tp_vars->last_sent,
					 packet_len,
					 tp_vars->socket_client->index,
					 jiffies_to_msecs(jiffies));

		/* something went wrong during the preparation/transmission */
		if (unlikely(err && err != BATADV_TP_CANT_SEND)) {
			batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
				   "Meter: batadv_tp_send() cannot send packets (%d)\n",
				   err);
			/* ensure nobody else tries to stop the thread now */
			if (atomic_dec_and_test(&tp_vars->sending))
				tp_vars->reason = err;
			break;
		}

		/* right-shift the TWND */
		if (!err)
			tp_vars->last_sent += payload_len;

		if (need_resched())
			schedule();
		else
			cpu_relax();
	}

out:
	if (likely(primary_if))
		batadv_hardif_put(primary_if);
	if (likely(orig_node))
		batadv_orig_node_put(orig_node);

	batadv_tp_finish(bat_priv, tp_vars);

	batadv_tp_vars_put(tp_vars);

	do_exit(0);
}

/**
 * batadv_tp_updated_cwnd - update the Congestion Windows
 * @tp_vars: the private data of the current TP meter session
 * @mss: TODO
 *
 * 1) if the session is in Slow Start, the CWND has to be increased by 1
 * MSS every unique received ACK
 * 2) if the session is in Congestion Avoidance, the CWND has to be
 * increased by MSS * MSS / CWND for every unique received ACK
 */
static void batadv_tp_update_cwnd(struct batadv_tp_vars *tp_vars, u32 mss)
{
	spin_lock_bh(&tp_vars->cwnd_lock);

	/* slow start... */
	if (tp_vars->cwnd <= tp_vars->ss_threshold) {
		tp_vars->dec_cwnd = 0;
		tp_vars->cwnd = batadv_tp_cwnd(tp_vars->cwnd, mss, mss);
		spin_unlock_bh(&tp_vars->cwnd_lock);
		return;
	}

	/* increment CWND at least of 1 (section 3.1 of RFC5681) */
	tp_vars->dec_cwnd += max_t(u32, 1U << 3,
				   ((mss * mss) << 6) / (tp_vars->cwnd << 3));
	if (tp_vars->dec_cwnd < (mss << 3)) {
		spin_unlock_bh(&tp_vars->cwnd_lock);
		return;
	}

	tp_vars->cwnd = batadv_tp_cwnd(tp_vars->cwnd, mss, mss);
	tp_vars->dec_cwnd = 0;

	spin_unlock_bh(&tp_vars->cwnd_lock);
}

/**
 * batadv_tp_recv_ack - ACK receiving function
 * @bat_priv: the bat priv with all the soft interface information
 * @skb: the buffer containing the received packet
 *
 * Process a recived TP ACK packet
 */
static void batadv_tp_recv_ack(struct batadv_priv *bat_priv,
			       struct sk_buff *skb)
{
	struct batadv_hard_iface *primary_if = NULL;
	struct batadv_orig_node *orig_node = NULL;
	struct batadv_icmp_tp_packet *icmp;
	struct batadv_tp_vars *tp_vars;
	size_t packet_len, mss;
	u32 rtt, recv_ack, cwnd;
	unsigned char *dev_addr;

	packet_len = PACKET_LEN;
	mss = PACKET_LEN;
	packet_len += sizeof(struct batadv_unicast_packet);

	icmp = (struct batadv_icmp_tp_packet *)skb->data;

	/* find the tp_vars */
	tp_vars = batadv_tp_list_find(bat_priv, icmp->orig);
	if (unlikely(!tp_vars))
		return;

	if (unlikely(atomic_read(&tp_vars->sending) == 0))
		goto out;

	/* old ACK? silently drop it.. */
	if (batadv_seq_before(ntohl(icmp->seqno),
			      (u32)atomic_read(&tp_vars->last_acked)))
		goto out;

	primary_if = batadv_primary_if_get_selected(bat_priv);
	if (unlikely(!primary_if))
		goto out;

	orig_node = batadv_orig_hash_find(bat_priv, icmp->orig);
	if (unlikely(!orig_node))
		goto out;

	/* update RTO with the new sampled RTT, if any */
	rtt = jiffies_to_msecs(jiffies) - ntohl(icmp->timestamp);
	if (icmp->timestamp && rtt)
		batadv_tp_update_rto(tp_vars, rtt);

	/* ACK for new data... reset the timer */
	batadv_tp_reset_sender_timer(tp_vars);

	recv_ack = ntohl(icmp->seqno);

	/* check if this ACK is a duplicate */
	if (atomic_read(&tp_vars->last_acked) == recv_ack) {
		atomic_inc(&tp_vars->dup_acks);
		if (atomic_read(&tp_vars->dup_acks) != 3)
			goto out;

		if (recv_ack >= tp_vars->recover)
			goto out;

		/* if this is the third duplicate ACK do Fast Retransmit */
		batadv_tp_send_msg(primary_if->net_dev->dev_addr,
				   orig_node, recv_ack, packet_len,
				   icmp->uid, jiffies_to_msecs(jiffies));

		spin_lock_bh(&tp_vars->cwnd_lock);

		/* Fast Recovery */
		tp_vars->fast_recovery = true;
		/* Set recover to the last outstanding seqno when Fast Recovery
		 * is entered. RFC6582, Section 3.2, step 1
		 */
		tp_vars->recover = tp_vars->last_sent;
		tp_vars->ss_threshold = tp_vars->cwnd >> 1;
		batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
			   "Meter: Fast Recovery, (cur cwnd=%u) ss_thr=%u last_sent=%u recv_ack=%u\n",
			   tp_vars->cwnd, tp_vars->ss_threshold,
			   tp_vars->last_sent, recv_ack);
		tp_vars->cwnd = batadv_tp_cwnd(tp_vars->ss_threshold, 3 * mss,
					       mss);
		tp_vars->dec_cwnd = 0;
		tp_vars->last_sent = recv_ack;

		spin_unlock_bh(&tp_vars->cwnd_lock);
	} else {
		/* count the acked data */
		atomic_add(recv_ack - atomic_read(&tp_vars->last_acked),
			   &tp_vars->tot_sent);
		/* reset the duplicate ACKs counter */
		atomic_set(&tp_vars->dup_acks, 0);

		if (tp_vars->fast_recovery) {
			/* partial ACK */
			if (batadv_seq_before(recv_ack, tp_vars->recover)) {
				/* this is another hole in the window. React
				 * immediately as specified by NewReno (see
				 * Section 3.2 of RFC6582 for details)
				 */
				dev_addr = primary_if->net_dev->dev_addr;
				batadv_tp_send_msg(dev_addr,
						   orig_node, recv_ack,
						   packet_len, icmp->uid,
						   jiffies_to_msecs(jiffies));
				tp_vars->cwnd = batadv_tp_cwnd(tp_vars->cwnd,
							       mss, mss);
			} else {
				tp_vars->fast_recovery = false;
				/* set cwnd to the value of ss_threshold at the
				 * moment that Fast Recovery was entered.
				 * RFC6582, Section 3.2, step 3
				 */
				cwnd = batadv_tp_cwnd(tp_vars->ss_threshold, 0,
						      mss);
				tp_vars->cwnd = cwnd;
			}
			goto move_twnd;
		}

		if (recv_ack - atomic_read(&tp_vars->last_acked) >= mss)
			batadv_tp_update_cwnd(tp_vars, mss);
move_twnd:
		/* move the Transmit Window */
		atomic_set(&tp_vars->last_acked, recv_ack);
	}

	wake_up(&tp_vars->more_bytes);
out:
	if (likely(primary_if))
		batadv_hardif_put(primary_if);
	if (likely(orig_node))
		batadv_orig_node_put(orig_node);
	if (likely(tp_vars))
		batadv_tp_vars_put(tp_vars);
}

/**
 * batadv_tp_sender_timeout - timer that fires in case of packet loss
 * @arg: address of the related tp_vars
 *
 * If fired it means that there was packet loss.
 * Switch to Slow Start, set the ss_threshold to half of the current cwnd and
 * reset the cwnd to 3*MSS
 */
static void batadv_tp_sender_timeout(unsigned long arg)
{
	struct batadv_tp_vars *tp_vars = (struct batadv_tp_vars *)arg;
	struct batadv_priv *bat_priv = tp_vars->bat_priv;

	if (atomic_read(&tp_vars->sending) == 0)
		return;

	/* if the user waited long enough...shutdown the test */
	if (unlikely(tp_vars->rto >= BATADV_TP_MAX_RTO)) {
		batadv_tp_shutdown(tp_vars, BATADV_TP_DST_UNREACHABLE);
		return;
	}

	/* RTO exponential backoff
	 * Details in Section 5.5 of RFC6298
	 */
	tp_vars->rto <<= 1;

	spin_lock_bh(&tp_vars->cwnd_lock);

	tp_vars->ss_threshold = tp_vars->cwnd >> 1;
	if (tp_vars->ss_threshold < PACKET_LEN * 2)
		tp_vars->ss_threshold = PACKET_LEN * 2;

	batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
		   "Meter: RTO fired during test towards %pM! cwnd=%u new ss_thr=%u, resetting last_sent to %u\n",
		   tp_vars->other_end, tp_vars->cwnd, tp_vars->ss_threshold,
		   atomic_read(&tp_vars->last_acked));

	tp_vars->cwnd = PACKET_LEN * 3;

	spin_unlock_bh(&tp_vars->cwnd_lock);

	/* resend the non-ACKed packets.. */
	tp_vars->last_sent = atomic_read(&tp_vars->last_acked);
	wake_up(&tp_vars->more_bytes);

	batadv_tp_reset_sender_timer(tp_vars);
}

/**
 * batadv_tp_stop - TODO
 * @bat_priv: TODO
 * @dst: TODO
 * @error_status: TODO
 */
void batadv_tp_stop(struct batadv_priv *bat_priv, u8 *dst,
		    u8 error_status)
{
	struct batadv_orig_node *orig_node;
	struct batadv_tp_vars *tp_vars;

	batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
		   "Meter: stopping test towards %pM\n", dst);

	orig_node = batadv_orig_hash_find(bat_priv, dst);
	if (!orig_node)
		return;

	tp_vars = batadv_tp_list_find(bat_priv, orig_node->orig);
	if (!tp_vars) {
		batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
			   "Meter: trying to interrupt an already over connection\n");
		goto out;
	}

	batadv_tp_shutdown(tp_vars, error_status);
	batadv_tp_vars_put(tp_vars);
out:
	batadv_orig_node_put(orig_node);
}

/**
 * batadv_tp_start_kthread - TODO
 * @tp_vars: TODO
 */
static void batadv_tp_start_kthread(struct batadv_tp_vars *tp_vars)
{
	struct task_struct *kthread;
	struct batadv_priv *bat_priv = tp_vars->bat_priv;

	kref_get(&tp_vars->refcount);
	kthread = kthread_create(batadv_tp_send, tp_vars, "kbatadv_tp_meter");
	if (IS_ERR(kthread)) {
		pr_err("batadv: cannot create tp meter kthread\n");
		batadv_tp_batctl_error_notify(BATADV_TP_MEMORY_ERROR,
					      tp_vars->socket_client->index);

		/* drop reserved reference for kthread */
		batadv_tp_vars_put(tp_vars);

		/* cleanup of failed tp meter variables */
		batadv_tp_cleanup(bat_priv, tp_vars);
		return;
	}

	wake_up_process(kthread);
}

/**
 * batadv_tp_start - TODO
 * @socket_client: TODO
 * @dst: TODO
 * @test_length: TODO
 */
void batadv_tp_start(struct batadv_socket_client *socket_client, u8 *dst,
		     u32 test_length)
{
	struct batadv_priv *bat_priv = socket_client->bat_priv;
	struct batadv_tp_vars *tp_vars;

	/* look for an already existing test towards this node */
	spin_lock_bh(&bat_priv->tp_list_lock);
	tp_vars = batadv_tp_list_find(bat_priv, dst);
	if (tp_vars) {
		spin_unlock_bh(&bat_priv->tp_list_lock);
		batadv_tp_vars_put(tp_vars);
		batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
			   "Meter: test to or from the same node already ongoing, aborting\n");
		batadv_tp_batctl_error_notify(BATADV_TP_ALREADY_ONGOING,
					      socket_client->index);
		return;
	}

	if (!atomic_add_unless(&bat_priv->tp_num, 1, BATADV_TP_MAX_NUM)) {
		spin_unlock_bh(&bat_priv->tp_list_lock);
		batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
			   "Meter: too many ongoing sessions, aborting (SEND)\n");
		batadv_tp_batctl_error_notify(BATADV_TP_TOO_MANY,
					      socket_client->index);
		return;
	}

	tp_vars = kmalloc(sizeof(*tp_vars), GFP_ATOMIC);
	if (!tp_vars) {
		spin_unlock_bh(&bat_priv->tp_list_lock);
		batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
			   "Meter: batadv_tp_start cannot allocate list elements\n");
		batadv_tp_batctl_error_notify(BATADV_TP_MEMORY_ERROR,
					      socket_client->index);
		return;
	}

	/* initialize tp_vars */
	ether_addr_copy(tp_vars->other_end, dst);
	kref_init(&tp_vars->refcount);
	tp_vars->status = BATADV_TP_SENDER;
	atomic_set(&tp_vars->sending, 1);

	tp_vars->last_sent = BATADV_TP_FIRST_SEQ;
	atomic_set(&tp_vars->last_acked, BATADV_TP_FIRST_SEQ);
	tp_vars->fast_recovery = false;
	tp_vars->recover = BATADV_TP_FIRST_SEQ;

	/* initialise the CWND to 3*MSS (Section 3.1 in RFC5681).
	 * For batman-adv the MSS is the size of the payload received by the
	 * soft_interface, hence its MTU
	 */
	tp_vars->cwnd = PACKET_LEN * 3;
	/* at the beginning initialise the SS threshold to the biggest possible
	 * window size, hence the AWND size
	 */
	tp_vars->ss_threshold = BATADV_TP_AWND;

	/* RTO initial value is 3 seconds.
	 * Details in Section 2.1 of RFC6298
	 */
	tp_vars->rto = 1000;
	tp_vars->srtt = 0;
	tp_vars->rttvar = 0;

	atomic_set(&tp_vars->tot_sent, 0);

	kref_get(&tp_vars->refcount);
	setup_timer(&tp_vars->timer, batadv_tp_sender_timeout,
		    (unsigned long)tp_vars);

	tp_vars->bat_priv = bat_priv;
	tp_vars->socket_client = socket_client;
	tp_vars->start_time = jiffies;

	init_waitqueue_head(&tp_vars->more_bytes);
	init_completion(&tp_vars->done);

	spin_lock_init(&tp_vars->unacked_lock);
	INIT_LIST_HEAD(&tp_vars->unacked_list);

	spin_lock_init(&tp_vars->cwnd_lock);

	kref_get(&tp_vars->refcount);
	hlist_add_head_rcu(&tp_vars->list, &bat_priv->tp_list);
	spin_unlock_bh(&bat_priv->tp_list_lock);

	tp_vars->test_length = test_length;
	if (!tp_vars->test_length)
		tp_vars->test_length = BATADV_TP_DEF_TEST_LENGTH;

	batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
		   "Meter: starting throughput meter towards %pM (length=%ums)\n",
		   dst, test_length);

	/* init work item for finished tp tests */
	INIT_DELAYED_WORK(&tp_vars->finish_work, batadv_tp_sender_finish);

	/* start tp kthread. This way the write() call issued from userspace can
	 * happily return and avoid to block
	 */
	batadv_tp_start_kthread(tp_vars);

	/* don't return reference to new tp_vars */
	batadv_tp_vars_put(tp_vars);
}

/**
 * batadv_tp_meter_recv - main TP Meter receiving function
 * @bat_priv: the bat priv with all the soft interface information
 * @skb: the received packet
 */
void batadv_tp_meter_recv(struct batadv_priv *bat_priv, struct sk_buff *skb)
{
	struct batadv_icmp_tp_packet *icmp;

	icmp = (struct batadv_icmp_tp_packet *)skb->data;

	switch (icmp->subtype) {
	case BATADV_TP_MSG:
		batadv_tp_recv_msg(bat_priv, skb);
		break;
	case BATADV_TP_ACK:
		batadv_tp_recv_ack(bat_priv, skb);
		break;
	default:
		batadv_dbg(BATADV_DBG_TP_METER, bat_priv,
			   "Received unknown TP Metric packet type %u\n",
			   icmp->subtype);
	}
	consume_skb(skb);
}