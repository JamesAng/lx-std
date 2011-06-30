/*
 * OMAP4 PRM module functions
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2010 Nokia Corporation
 * Benoît Cousson
 * Paul Walmsley
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/io.h>

#include <plat/common.h>
#include <plat/cpu.h>
#include <plat/prcm.h>

#include "vp.h"
#include "prm44xx.h"
#include "prm-regbits-44xx.h"
#include "prcm44xx.h"
#include "prminst44xx.h"

/* PRM low-level functions */

/* Read a register in a CM/PRM instance in the PRM module */
u32 omap4_prm_read_inst_reg(s16 inst, u16 reg)
{
	return __raw_readl(OMAP44XX_PRM_REGADDR(inst, reg));
}

/* Write into a register in a CM/PRM instance in the PRM module */
void omap4_prm_write_inst_reg(u32 val, s16 inst, u16 reg)
{
	__raw_writel(val, OMAP44XX_PRM_REGADDR(inst, reg));
}

/* Read-modify-write a register in a PRM module. Caller must lock */
u32 omap4_prm_rmw_inst_reg_bits(u32 mask, u32 bits, s16 inst, s16 reg)
{
	u32 v;

	v = omap4_prm_read_inst_reg(inst, reg);
	v &= ~mask;
	v |= bits;
	omap4_prm_write_inst_reg(v, inst, reg);

	return v;
}

/* PRM VP */

/*
 * struct omap4_prm_irq - OMAP4 VP register access description.
 * @irqstatus_mpu: offset to IRQSTATUS_MPU register for VP
 * @vp_tranxdone_status: VP_TRANXDONE_ST bitmask in PRM_IRQSTATUS_MPU reg
 * @abb_tranxdone_status: ABB_TRANXDONE_ST bitmask in PRM_IRQSTATUS_MPU reg
 */
struct omap4_prm_irq {
	u32 irqstatus_mpu;
	u32 vp_tranxdone_status;
	u32 abb_tranxdone_status;
};

static struct omap4_prm_irq omap4_prm_irqs[] = {
	[OMAP4_PRM_IRQ_VDD_MPU_ID] = {
		.irqstatus_mpu = OMAP4_PRM_IRQSTATUS_MPU_2_OFFSET,
		.vp_tranxdone_status = OMAP4430_VP_MPU_TRANXDONE_ST_MASK,
		.abb_tranxdone_status = OMAP4430_ABB_MPU_DONE_ST_MASK
	},
	[OMAP4_PRM_IRQ_VDD_IVA_ID] = {
		.irqstatus_mpu = OMAP4_PRM_IRQSTATUS_MPU_OFFSET,
		.vp_tranxdone_status = OMAP4430_VP_IVA_TRANXDONE_ST_MASK,
		.abb_tranxdone_status = OMAP4430_ABB_IVA_DONE_ST_MASK,
	},
	[OMAP4_PRM_IRQ_VDD_CORE_ID] = {
		.irqstatus_mpu = OMAP4_PRM_IRQSTATUS_MPU_OFFSET,
		.vp_tranxdone_status = OMAP4430_VP_CORE_TRANXDONE_ST_MASK,
		/* Core has no ABB */
	},
};

u32 omap4_prm_vp_check_txdone(u8 irq_id)
{
	struct omap4_prm_irq *irq = &omap4_prm_irqs[irq_id];
	u32 irqstatus;

	irqstatus = omap4_prminst_read_inst_reg(OMAP4430_PRM_PARTITION,
						OMAP4430_PRM_OCP_SOCKET_INST,
						irq->irqstatus_mpu);
	return irqstatus & irq->vp_tranxdone_status;
}

void omap4_prm_vp_clear_txdone(u8 irq_id)
{
	struct omap4_prm_irq *irq = &omap4_prm_irqs[irq_id];

	omap4_prminst_write_inst_reg(irq->vp_tranxdone_status,
				     OMAP4430_PRM_PARTITION,
				     OMAP4430_PRM_OCP_SOCKET_INST,
				     irq->irqstatus_mpu);
};

u32 omap4_prm_abb_check_txdone(u8 irq_id)
{
	struct omap4_prm_irq *irq = &omap4_prm_irqs[irq_id];
	u32 irqstatus;

	irqstatus = omap4_prminst_read_inst_reg(OMAP4430_PRM_PARTITION,
						OMAP4430_PRM_OCP_SOCKET_INST,
						irq->irqstatus_mpu);
	return irqstatus & irq->abb_tranxdone_status;
}

void omap4_prm_abb_clear_txdone(u8 irq_id)
{
	struct omap4_prm_irq *irq = &omap4_prm_irqs[irq_id];

	omap4_prminst_write_inst_reg(irq->abb_tranxdone_status,
				     OMAP4430_PRM_PARTITION,
				     OMAP4430_PRM_OCP_SOCKET_INST,
				     irq->irqstatus_mpu);
}

u32 omap4_prm_vcvp_read(u8 offset)
{
	return omap4_prminst_read_inst_reg(OMAP4430_PRM_PARTITION,
					   OMAP4430_PRM_DEVICE_INST, offset);
}

void omap4_prm_vcvp_write(u32 val, u8 offset)
{
	omap4_prminst_write_inst_reg(val, OMAP4430_PRM_PARTITION,
				     OMAP4430_PRM_DEVICE_INST, offset);
}

u32 omap4_prm_vcvp_rmw(u32 mask, u32 bits, u8 offset)
{
	return omap4_prminst_rmw_inst_reg_bits(mask, bits,
					       OMAP4430_PRM_PARTITION,
					       OMAP4430_PRM_DEVICE_INST,
					       offset);
}
