/*
 * Copyright (C) 2010 B.A.T.M.A.N. contributors:
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

#ifndef _NET_BATMAN_ADV_NDP_H_
#define _NET_BATMAN_ADV_NDP_H_

void ndp_start_timer(struct batman_if *batman_if);
void ndp_stop_timer(struct batman_if *batman_if);

int ndp_init(struct batman_if *batman_if);
void ndp_free(struct batman_if *batman_if);
uint8_t ndp_fetch_tq(struct batman_packet_ndp *packet,
		 uint8_t *my_if_addr);
void ndp_purge_neighbors(void);
int ndp_update_neighbor(uint8_t my_tq, uint32_t seqno,
			struct batman_if *batman_if, uint8_t *neigh_addr);
int ndp_seq_print_text(struct seq_file *seq, void *offset);

#endif /* _NET_BATMAN_ADV_NDP_H_ */