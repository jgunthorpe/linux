/*
 * arch/arm/plat-orion/irq.c
 *
 * Marvell Orion SoC IRQ handling.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <plat/irq.h>
#include <plat/orion-gpio.h>

static void bridge_irq_handler(unsigned irq, struct irq_desc *desc)
{
	struct irq_chip_generic *gc = irq_get_handler_data(irq);
	u32 cause;
	int i;

	cause = readl(gc->reg_base) & readl(gc->reg_base + 4);
	for (i = 0; i < 6; i++)
		if ((cause & (1 << i)))
			generic_handle_irq(i + gc->irq_base);
}

static void irq_gc_eoi_inv(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	u32 mask = 1 << (d->irq - gc->irq_base);
	struct irq_chip_regs *regs;

	regs = &container_of(d->chip, struct irq_chip_type, chip)->regs;
	irq_gc_lock(gc);
	irq_reg_writel(~mask, gc->reg_base + regs->eoi);
	irq_gc_unlock(gc);
}

void __init orion_bridge_irq_init(unsigned int bridge_irq,
				  unsigned int irq_start,
				  void __iomem *causeaddr)
{
	struct irq_chip_generic *gc;
	struct irq_chip_type *ct;

	gc = irq_alloc_generic_chip("orion_irq_edge", 1, irq_start,
				    causeaddr, handle_fasteoi_irq);
	if (!gc)
		BUG();
	ct = gc->chip_types;
	ct->regs.mask = 4;
	ct->regs.eoi = 0;
	/* ACK and mask all interrupts */
	writel(0, causeaddr);
	writel(0, causeaddr + ct->regs.mask);
	ct->chip.irq_eoi = irq_gc_eoi_inv;
	ct->chip.irq_mask = irq_gc_mask_clr_bit;
	ct->chip.irq_unmask = irq_gc_mask_set_bit;
	irq_setup_generic_chip(gc, IRQ_MSK(6), IRQ_GC_INIT_MASK_CACHE,
			       IRQ_NOREQUEST, IRQ_LEVEL | IRQ_NOPROBE);
	if (irq_set_handler_data(bridge_irq, gc) != 0)
		BUG();
	irq_set_chained_handler(bridge_irq, bridge_irq_handler);
}

void __init orion_irq_init(unsigned int irq_start, void __iomem *maskaddr)
{
	struct irq_chip_generic *gc;
	struct irq_chip_type *ct;

	/*
	 * Mask all interrupts initially.
	 */
	writel(0, maskaddr);

	gc = irq_alloc_generic_chip("orion_irq", 1, irq_start, maskaddr,
				    handle_level_irq);
	ct = gc->chip_types;
	ct->chip.irq_mask = irq_gc_mask_clr_bit;
	ct->chip.irq_unmask = irq_gc_mask_set_bit;
	irq_setup_generic_chip(gc, IRQ_MSK(32), IRQ_GC_INIT_MASK_CACHE,
			       IRQ_NOREQUEST, IRQ_LEVEL | IRQ_NOPROBE);
}

#ifdef CONFIG_OF
static int __init orion_add_irq_domain(struct device_node *np,
				       struct device_node *interrupt_parent)
{
	int i = 0, irq_gpio;
	void __iomem *base;

	do {
		base = of_iomap(np, i);
		if (base) {
			orion_irq_init(i * 32, base);
			i++;
		}
	} while (base);

	irq_domain_add_legacy(np, i * 32, 0, 0,
			      &irq_domain_simple_ops, NULL);

	irq_gpio = i * 32;
	orion_gpio_of_init(irq_gpio);

	return 0;
}

static const struct of_device_id orion_irq_match[] = {
	{ .compatible = "marvell,orion-intc",
	  .data = orion_add_irq_domain, },
	{},
};

void __init orion_dt_init_irq(void)
{
	of_irq_init(orion_irq_match);
}
#endif
