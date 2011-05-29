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

#ifndef _NET_BATMAN_ADV_NDP_H_
#define _NET_BATMAN_ADV_NDP_H_

void ndp_start_timer(struct hard_iface *hard_iface);
void ndp_stop_timer(struct hard_iface *hard_iface);

int ndp_init(struct hard_iface *hard_iface);
void ndp_free(struct hard_iface *hard_iface);

#endif /* _NET_BATMAN_ADV_NDP_H_ */
