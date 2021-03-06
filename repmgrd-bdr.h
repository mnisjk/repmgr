/*
 * repmgrd-bdr.h
 * Copyright (c) 2ndQuadrant, 2010-2018
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _REPMGRD_BDR_H_
#define _REPMGRD_BDR_H_

extern void do_bdr_node_check(void);
extern void monitor_bdr(void);

extern void	handle_sigint_bdr(SIGNAL_ARGS);
#endif							/* _REPMGRD_BDR_H_ */
