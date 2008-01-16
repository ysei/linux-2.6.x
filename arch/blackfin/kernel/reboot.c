/*
 * arch/blackfin/kernel/reboot.c - handle shutdown/reboot
 *
 * Copyright 2004-2007 Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/interrupt.h>
#include <asm/bfin-global.h>
#include <asm/reboot.h>
#include <asm/system.h>

#if defined(BF537_FAMILY) || defined(BF533_FAMILY) || defined(BF527_FAMILY)
#define SYSCR_VAL 	0x0
#elif defined(BF561_FAMILY)
#define SYSCR_VAL 	0x20
#elif defined(BF548_FAMILY)
#define SYSCR_VAL 	0x10
#endif

/*
 * Delay min 5 SCLK cycles using worst case CCLK/SCLK ratio (15)
 */
#define SWRST_DELAY	(5 * 15)

/* A system soft reset makes external memory unusable
 * so force this function into L1.
 */
__attribute__((l1_text))
void bfin_reset(void)
{
	int cntr;
	/* force BMODE and disable Core B (as needed) */
	bfin_write_SYSCR(SYSCR_VAL);

	/* we use asm ssync here because it's save and we save some L1 */
	asm("ssync;");

	while (1) {
		/* initiate system soft reset with magic 0x7 */
		bfin_write_SWRST(0x7);
		for (cntr = 0; cntr < SWRST_DELAY; cntr++)
			asm("NOP;");
		/* clear system soft reset */
		bfin_write_SWRST(0);
		for (cntr = 0; cntr < SWRST_DELAY; cntr++)
			asm("NOP;");
		/* issue core reset */
		asm("raise 1");
	}
}

__attribute__((weak))
void native_machine_restart(char *cmd)
{
}

void machine_restart(char *cmd)
{
	native_machine_restart(cmd);
	local_irq_disable();
	bfin_reset();
}

__attribute__((weak))
void native_machine_halt(void)
{
	idle_with_irq_disabled();
}

void machine_halt(void)
{
	native_machine_halt();
}

__attribute__((weak))
void native_machine_power_off(void)
{
	idle_with_irq_disabled();
}

void machine_power_off(void)
{
	native_machine_power_off();
}
