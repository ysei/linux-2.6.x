/* $Id$
 *
 * Ansteuerung ARCOFI 2165
 *
 * Author       Karsten Keil
 * Copyright    by Karsten Keil      <keil@isdn4linux.de>
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 */
 
#include "hisax.h"
#include "isdnl1.h"
#include "isac.h"
#include "arcofi.h"

#define ARCOFI_TIMER_VALUE	20


static inline u8
isac_read(struct IsdnCardState *cs, u8 addr)
{
	return cs->dc_hw_ops->read_reg(cs, addr);
}

static inline void
isac_write(struct IsdnCardState *cs, u8 addr, u8 val)
{
	cs->dc_hw_ops->write_reg(cs, addr, val);
}

static void
add_arcofi_timer(struct IsdnCardState *cs) {
	if (test_and_set_bit(FLG_ARCOFI_TIMER, &cs->HW_Flags)) {
		del_timer(&cs->dc.isac.arcofitimer);
	}	
	init_timer(&cs->dc.isac.arcofitimer);
	cs->dc.isac.arcofitimer.expires = jiffies + ((ARCOFI_TIMER_VALUE * HZ)/1000);
	add_timer(&cs->dc.isac.arcofitimer);
}

static void
send_arcofi(struct IsdnCardState *cs) {
	u8 val;
	
	add_arcofi_timer(cs);
	cs->dc.isac.mon_txp = 0;
	cs->dc.isac.mon_txc = cs->dc.isac.arcofi_list->len;
	memcpy(cs->dc.isac.mon_tx, cs->dc.isac.arcofi_list->msg, cs->dc.isac.mon_txc);
	switch(cs->dc.isac.arcofi_bc) {
		case 0: break;
		case 1: cs->dc.isac.mon_tx[1] |= 0x40;
			break;
		default: break;
	}
	cs->dc.isac.mocr &= 0x0f;
	cs->dc.isac.mocr |= 0xa0;
	isac_write(cs, ISAC_MOCR, cs->dc.isac.mocr);
	val = isac_read(cs, ISAC_MOSR);
	isac_write(cs, ISAC_MOX1, cs->dc.isac.mon_tx[cs->dc.isac.mon_txp++]);
	cs->dc.isac.mocr |= 0x10;
	isac_write(cs, ISAC_MOCR, cs->dc.isac.mocr);
}

int
arcofi_fsm(struct IsdnCardState *cs, int event, void *data) {
	if (cs->debug & L1_DEB_MONITOR) {
		debugl1(cs, "arcofi state %d event %d", cs->dc.isac.arcofi_state, event);
	}
	if (event == ARCOFI_TIMEOUT) {
		cs->dc.isac.arcofi_state = ARCOFI_NOP;
		test_and_set_bit(FLG_ARCOFI_ERROR, &cs->HW_Flags);
		wake_up(&cs->dc.isac.arcofi_wait);
 		return(1);
	}
	switch (cs->dc.isac.arcofi_state) {
		case ARCOFI_NOP:
			if (event == ARCOFI_START) {
				cs->dc.isac.arcofi_list = data;
				cs->dc.isac.arcofi_state = ARCOFI_TRANSMIT;
				send_arcofi(cs);
			}
			break;
		case ARCOFI_TRANSMIT:
			if (event == ARCOFI_TX_END) {
				if (cs->dc.isac.arcofi_list->receive) {
					add_arcofi_timer(cs);
					cs->dc.isac.arcofi_state = ARCOFI_RECEIVE;
				} else {
					if (cs->dc.isac.arcofi_list->next) {
						cs->dc.isac.arcofi_list =
							cs->dc.isac.arcofi_list->next;
						send_arcofi(cs);
					} else {
						if (test_and_clear_bit(FLG_ARCOFI_TIMER, &cs->HW_Flags)) {
							del_timer(&cs->dc.isac.arcofitimer);
						}
						cs->dc.isac.arcofi_state = ARCOFI_NOP;
						wake_up(&cs->dc.isac.arcofi_wait);
					}
				}
			}
			break;
		case ARCOFI_RECEIVE:
			if (event == ARCOFI_RX_END) {
				if (cs->dc.isac.arcofi_list->next) {
					cs->dc.isac.arcofi_list =
						cs->dc.isac.arcofi_list->next;
					cs->dc.isac.arcofi_state = ARCOFI_TRANSMIT;
					send_arcofi(cs);
				} else {
					if (test_and_clear_bit(FLG_ARCOFI_TIMER, &cs->HW_Flags)) {
						del_timer(&cs->dc.isac.arcofitimer);
					}
					cs->dc.isac.arcofi_state = ARCOFI_NOP;
					wake_up(&cs->dc.isac.arcofi_wait);
				}
			}
			break;
		default:
			debugl1(cs, "Arcofi unknown state %x", cs->dc.isac.arcofi_state);
			return(2);
	}
	return(0);
}

static void
arcofi_timer(struct IsdnCardState *cs) {
	arcofi_fsm(cs, ARCOFI_TIMEOUT, NULL);
}

void
clear_arcofi(struct IsdnCardState *cs) {
	if (test_and_clear_bit(FLG_ARCOFI_TIMER, &cs->HW_Flags)) {
		del_timer(&cs->dc.isac.arcofitimer);
	}
}

void
init_arcofi(struct IsdnCardState *cs) {
	cs->dc.isac.arcofitimer.function = (void *) arcofi_timer;
	cs->dc.isac.arcofitimer.data = (long) cs;
	init_timer(&cs->dc.isac.arcofitimer);
	init_waitqueue_head(&cs->dc.isac.arcofi_wait);
	test_and_set_bit(HW_ARCOFI, &cs->HW_Flags);
}
