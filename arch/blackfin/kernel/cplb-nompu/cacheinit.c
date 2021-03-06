/*
 *               Copyright 2004-2007 Analog Devices Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see the file COPYING, or write
 * to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <linux/cpu.h>

#include <asm/cacheflush.h>
#include <asm/blackfin.h>
#include <asm/cplb.h>
#include <asm/cplbinit.h>

#if defined(CONFIG_BFIN_ICACHE)
void __cpuinit bfin_icache_init(struct cplb_entry *icplb_tbl)
{
	unsigned long ctrl;
	int i;

	for (i = 0; i < MAX_CPLBS; i++) {
		bfin_write32(ICPLB_ADDR0 + i * 4, icplb_tbl[i].addr);
		bfin_write32(ICPLB_DATA0 + i * 4, icplb_tbl[i].data);
	}
	ctrl = bfin_read_IMEM_CONTROL();
	ctrl |= IMC | ENICPLB;
	/* CSYNC to ensure load store ordering */
	CSYNC();
	bfin_write_IMEM_CONTROL(ctrl);
	SSYNC();
}
#endif

#if defined(CONFIG_BFIN_DCACHE)
void __cpuinit bfin_dcache_init(struct cplb_entry *dcplb_tbl)
{
	unsigned long ctrl;
	int i;

	for (i = 0; i < MAX_CPLBS; i++) {
		bfin_write32(DCPLB_ADDR0 + i * 4, dcplb_tbl[i].addr);
		bfin_write32(DCPLB_DATA0 + i * 4, dcplb_tbl[i].data);
	}

	ctrl = bfin_read_DMEM_CONTROL();

	/*
	 *  Anomaly notes:
	 *  05000287 - We implement workaround #2 - Change the DMEM_CONTROL
	 *  register, so that the port preferences for DAG0 and DAG1 are set
	 *  to port B
	 */
	ctrl |= DMEM_CNTR | PORT_PREF0 | (ANOMALY_05000287 ? PORT_PREF1 : 0);
	/* CSYNC to ensure load store ordering */
	CSYNC();
	bfin_write_DMEM_CONTROL(ctrl);
	SSYNC();
}
#endif
