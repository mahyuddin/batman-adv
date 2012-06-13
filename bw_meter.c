#include "main.h"
#include "send.h"
#include "hash.h"
#include "originator.h"
#include "hard-interface.h"
#include "bw_meter.h"
#include "icmp_socket.h"
#include "types.h"
#include "bw_meter.h"

#define BW_PACKET_LEN 1500
#define BW_WINDOW_SIZE 300
#define BW_CLEAN_RECEIVER_TIMEOUT 2000
#define BW_TIMEOUT 800
#define BW_WORKER_TIMEOUT (BW_TIMEOUT/10)

int send_icmp_packet(struct batadv_priv *bat_priv, struct sk_buff *skb)
{
	struct batadv_hard_iface *primary_if = NULL;
	struct batadv_orig_node *orig_node = NULL;
	struct batadv_neigh_node *neigh_node = NULL;
	struct batadv_icmp_packet *icmp_packet;
	int ret = -1;

	icmp_packet = (struct batadv_icmp_packet *)skb->data;

	primary_if = batadv_primary_if_get_selected(bat_priv);
	if (!primary_if) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter:send_icmp_packet: no primary if\n");
		goto out;
	}
	if (atomic_read(&bat_priv->mesh_state) != BATADV_MESH_ACTIVE) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter:send_icmp_packet: mesh inactive\n");
		goto dst_unreach;
	}

	orig_node = batadv_orig_hash_find(bat_priv,
					  icmp_packet->dst);
	if (!orig_node) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter:send_icmp_packet: no orig node\n");
		goto dst_unreach;
	}

	neigh_node = batadv_orig_node_get_router(orig_node);
	if (!neigh_node) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter:send_icmp_packet: no neigh node\n");
		goto dst_unreach;
	}

	if (!neigh_node->if_incoming) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter:send_icmp_packet: no if incoming\n");
		goto dst_unreach;
	}

	if (neigh_node->if_incoming->if_status != BATADV_IF_ACTIVE) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter:send_icmp_packet: status not IF_ACTIVE\n");
		goto dst_unreach;
	}

	memcpy(icmp_packet->orig,
	       primary_if->net_dev->dev_addr, ETH_ALEN);

	batadv_send_skb_packet(skb, neigh_node->if_incoming,
			       neigh_node->addr);
	ret = 0;
	goto out;

dst_unreach:
	/* icmp_to_send->msg_type = DESTINATION_UNREACHABLE;
	batadv_socket_add_packet(socket_client, icmp_to_send, packet_len);
	 */

out:
	if (primary_if)
		batadv_hardif_free_ref(primary_if);
	if (neigh_node)
		batadv_neigh_node_free_ref(neigh_node);
	if (orig_node)
		batadv_orig_node_free_ref(orig_node);
	return ret;
}

int batadv_send_bw_ack(struct batadv_socket_client *socket_client,
		       struct batadv_icmp_packet *icmp_packet,  int seq)
{
	struct sk_buff *skb;
	struct icmp_packet_bw *icmp_ack;
	struct batadv_priv *bat_priv = socket_client->bat_priv;
	int ret = -1;

	bat_priv = socket_client->bat_priv;
	skb = dev_alloc_skb(sizeof(struct icmp_packet_bw) + ETH_HLEN);
	if (!skb) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter: batadv_send_bw_ack cannot allocate skb\n");
		goto out;
	}

	skb_reserve(skb, ETH_HLEN);
	icmp_ack = (struct icmp_packet_bw *)
		   skb_put(skb, sizeof(struct icmp_packet_bw));
	icmp_ack->header.packet_type = BATADV_ICMP;
	icmp_ack->header.version = BATADV_COMPAT_VERSION;
	icmp_ack->header.ttl = 50;
	icmp_ack->seqno = seq;
	icmp_ack->msg_type = BW_ACK;
	memcpy(icmp_ack->dst, icmp_packet->orig, ETH_ALEN);
	icmp_ack->uid = socket_client->index;

	/* send the ack */
	if (send_icmp_packet(bat_priv, skb) < 0) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter: batadv_send_bw_ack cannot send_icmp_packet\n");
		goto out;
	}
	ret = 0;
out:
	return ret;
}

void batadv_bw_receiver_clean(struct work_struct *work)
{
	struct delayed_work *delayed_work;
	struct batadv_priv *bat_priv;

	delayed_work = container_of(work, struct delayed_work, work);
	bat_priv = container_of(delayed_work, struct batadv_priv, bw_work);

	/* TODO deallocate struct */
	pr_info("test finished\n");
	bat_priv->bw_vars->status = INACTIVE;
}

void batadv_bw_meter_received(struct batadv_priv *bat_priv, struct sk_buff *skb)
{
	struct icmp_packet_bw *icmp_packet;
	struct batadv_socket_client *socket_client;
	int jiffies_timeout;

	icmp_packet = (struct icmp_packet_bw *)skb->data;
	jiffies_timeout = msecs_to_jiffies(BW_CLEAN_RECEIVER_TIMEOUT);
	socket_client = container_of(&bat_priv, struct batadv_socket_client,
				     bat_priv);
	batadv_dbg(BATADV_DBG_BATMAN, bat_priv, "Meter: BW_METER received\n");

	/* setup RECEIVER data structure */
	if (!bat_priv->bw_vars) {
		if (icmp_packet->seqno != 0) {
			batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
				   "Meter: seq != 0 cannot initiate connection\n");
			goto out;
		}
		bat_priv->bw_vars =
			kmalloc(sizeof(struct bw_vars), GFP_ATOMIC);
		if (!bat_priv->bw_vars) {
			batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
				   "Meter: meter_received cannot allocate bw_vars\n");
			goto out;
		}
		bat_priv->bw_vars->status = INACTIVE;
	}

	if (bat_priv->bw_vars->status == INACTIVE) {
		if (icmp_packet->seqno != 0)
			goto out;
		bat_priv->bw_vars->status = RECEIVER;
		bat_priv->bw_vars->window_first = 0;
		bat_priv->bw_vars->total_to_send = 0;
	}

	if (bat_priv->bw_vars->status != RECEIVER) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter: cannot be sender and receiver\n");
		goto out;
	}

	/* check if packet belongs to window */
	if (icmp_packet->seqno < bat_priv->bw_vars->window_first) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter: %d < window_first\n", icmp_packet->seqno);
		goto out;
	}

	if (icmp_packet->seqno >
	    bat_priv->bw_vars->window_first + BW_WINDOW_SIZE) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter: unexpected packet received\n");
		goto out;
	}

	if (icmp_packet->seqno == bat_priv->bw_vars->window_first) {
		bat_priv->bw_vars->window_first++;
		batadv_send_bw_ack(socket_client,
				   (struct batadv_icmp_packet *) icmp_packet,
				   icmp_packet->seqno);

		/* check for last packet */
		if (skb->len < BW_PACKET_LEN) {
			INIT_DELAYED_WORK(&bat_priv->bw_work,
					  batadv_bw_receiver_clean);
			queue_delayed_work(batadv_event_workqueue,
					   &bat_priv->bw_work,
					   jiffies_timeout);
		}
	}

	goto out;
out:
	return;
}

static void batadv_bw_worker(struct work_struct *work)
{
	struct delayed_work *delayed_work;
	struct batadv_priv *bat_priv;
	unsigned long int test_time;

	delayed_work = container_of(work, struct delayed_work, work);
	bat_priv = container_of(delayed_work, struct batadv_priv, bw_work);
	if (batadv_has_timed_out(bat_priv->bw_vars->last_sent_time,
				 BW_TIMEOUT)) {
		bat_priv->bw_vars->next_to_send =
			bat_priv->bw_vars->window_first;
		batadv_send_remaining_window(bat_priv);
	}

	/* if not finished, re-enqueue worker */
	if (bat_priv->bw_vars->window_first <
	    bat_priv->bw_vars->total_to_send) {
		queue_delayed_work(batadv_event_workqueue, &bat_priv->bw_work,
				   msecs_to_jiffies(BW_WORKER_TIMEOUT));
	} else {
		test_time = (long)jiffies - (long)bat_priv->bw_vars->start_time;
		pr_info("Meter: test over in %lu s. Throughput %lu B/s\n",
			test_time/HZ,
			bat_priv->bw_vars->total_to_send *
			BW_PACKET_LEN / (test_time/HZ));
	}
}

/* sends packets from next_to_send to (window_first+BW_WINDOW_SIZE) */
int batadv_send_remaining_window(struct batadv_priv *bat_priv)
{
	struct sk_buff *skb;
	struct icmp_packet_bw *icmp_to_send;
	int send_until, ret = -1, bw_packet_len = BW_PACKET_LEN;
	struct batadv_socket_client *socket_client;

	socket_client = container_of(&bat_priv, struct batadv_socket_client,
				     bat_priv);

	send_until = min(bat_priv->bw_vars->window_first + BW_WINDOW_SIZE,
			 bat_priv->bw_vars->total_to_send + 1);

	while (bat_priv->bw_vars->next_to_send < send_until) {
		if (bat_priv->bw_vars->next_to_send ==
		    bat_priv->bw_vars->total_to_send)
			bw_packet_len -= 1;

		skb = dev_alloc_skb(bw_packet_len + ETH_HLEN);
		if (!skb) {
			batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
				   "Meter: send_remaining_window() cannot allocate skb\n");
			goto out;
		}

		skb_reserve(skb, ETH_HLEN);
		icmp_to_send = (struct icmp_packet_bw *)skb_put(skb,
								bw_packet_len);

		/* fill the icmp header */
		memcpy(&icmp_to_send->dst, &bat_priv->bw_vars->other_end,
		       ETH_ALEN);
		icmp_to_send->header.version = BATADV_COMPAT_VERSION;
		icmp_to_send->header.packet_type = BATADV_ICMP;
		icmp_to_send->msg_type = BW_METER;
		icmp_to_send->seqno = bat_priv->bw_vars->next_to_send++;
		icmp_to_send->uid = socket_client->index;
		if (send_icmp_packet(bat_priv, skb) < 0) {
			batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
				   "Meter: send_remaining_window cannot send_icmp_packet\n");
			goto out;
		}
		bat_priv->bw_vars->last_sent_time = jiffies;
	}
	ret = 0;
out:
	return ret;
}

void batadv_bw_ack_received(struct batadv_priv *bat_priv, struct sk_buff *skb)
{
	struct icmp_packet_bw *icmp_packet = (struct icmp_packet_bw *)skb->data;

	if (icmp_packet->seqno < bat_priv->bw_vars->window_first) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter: received ack %d < window_first\n",
			   icmp_packet->seqno);
		goto out;
	}
	if (icmp_packet->seqno > bat_priv->bw_vars->next_to_send) {
		batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
			   "Meter: received ack %d > next_to_send\n",
			   icmp_packet->seqno);
		goto out;
	}
	bat_priv->bw_vars->window_first = icmp_packet->seqno + 1;
	batadv_send_remaining_window(bat_priv);
out:
	return;
}

void batadv_bw_start(struct batadv_priv *bat_priv,
		     struct icmp_packet_bw *icmp_packet_bw)
{
	batadv_dbg(BATADV_DBG_BATMAN, bat_priv, "Meter started...\n");

	/* check bw_vars */
	if (!bat_priv->bw_vars) {
		bat_priv->bw_vars = kmalloc(sizeof(struct bw_vars),
					    GFP_ATOMIC);
		if (!bat_priv->bw_vars)
			goto out;

		bat_priv->bw_vars->status = INACTIVE;
	}

	if (bat_priv->bw_vars->status != INACTIVE)
		goto out;

	memcpy(&bat_priv->bw_vars->other_end, &icmp_packet_bw->dst, ETH_ALEN);
	bat_priv->bw_vars->total_to_send = 3000;
	bat_priv->bw_vars->next_to_send = 0;
	bat_priv->bw_vars->window_first = 0;
	bat_priv->bw_vars->last_sent_time = jiffies;
	bat_priv->bw_vars->start_time = jiffies;

	INIT_DELAYED_WORK(&bat_priv->bw_work, batadv_bw_worker);
	queue_delayed_work(batadv_event_workqueue, &bat_priv->bw_work,
			   msecs_to_jiffies(BW_TIMEOUT));
	batadv_send_remaining_window(bat_priv);
	goto out;
out:
	return;
}
