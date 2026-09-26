// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#include <dt-bindings/mailbox/qcom-ipcc.h>

/* IPCC Register offsets */
#define IPCC_REG_CONFIG			0x08
#define IPCC_REG_SEND_ID		0x0c
#define IPCC_REG_RECV_ID		0x10
#define IPCC_REG_RECV_SIGNAL_ENABLE	0x14
#define IPCC_REG_RECV_SIGNAL_DISABLE	0x18
#define IPCC_REG_RECV_SIGNAL_CLEAR	0x1c
#define IPCC_REG_CLIENT_CLEAR		0x38

#define IPCC_CLEAR_ON_RECV_RD		BIT(0)
#define IPCC_SIGNAL_ID_MASK		GENMASK(15, 0)
#define IPCC_CLIENT_ID_MASK		GENMASK(31, 16)

#define IPCC_NO_PENDING_IRQ		GENMASK(31, 0)

/*
 * TEMP: wake trace for the deep suspend bring-up.  Off by default; to be
 * reverted once the source of the early wake-ups has been identified.
 */
static bool temp_wake_trace;
module_param(temp_wake_trace, bool, 0444);
MODULE_PARM_DESC(temp_wake_trace,
		 "TEMP: record the IPCC signals around system suspend (default: off)");

#define IPCC_WAKE_CYCLES		4
#define IPCC_WAKE_RECS			8
#define IPCC_WAKE_LINE			192
/* RECV_ID was not read because reading it would have cleared the signal */
#define IPCC_WAKE_UNREAD		0xfffffffe

enum qcom_ipcc_wake_point {
	IPCC_WAKE_NOIRQ_SUSPEND,
	IPCC_WAKE_SYSCORE_SUSPEND,
	IPCC_WAKE_SYSCORE_RESUME,
	IPCC_WAKE_NOIRQ_RESUME,
	IPCC_WAKE_POINTS,
};

static const char * const qcom_ipcc_wake_point_names[IPCC_WAKE_POINTS] = {
	[IPCC_WAKE_NOIRQ_SUSPEND] = "noirq-suspend",
	[IPCC_WAKE_SYSCORE_SUSPEND] = "syscore-suspend",
	[IPCC_WAKE_SYSCORE_RESUME] = "syscore-resume",
	[IPCC_WAKE_NOIRQ_RESUME] = "noirq-resume",
};

enum qcom_ipcc_wake_window {
	IPCC_WAKE_IDLE,
	IPCC_WAKE_PRE,		/* noirq suspend until syscore suspend */
	IPCC_WAKE_ASLEEP,	/* syscore suspend until syscore resume */
	IPCC_WAKE_POST,		/* syscore resume until complete */
};

struct qcom_ipcc_wake_rec {
	u64 ts;
	u32 hwirq;
	int virq;
	bool disabled;
};

struct qcom_ipcc_wake_cycle {
	u32 seq;
	bool seen[IPCC_WAKE_POINTS];
	u32 pending[IPCC_WAKE_POINTS];
	u64 pending_ts[IPCC_WAKE_POINTS];
	u32 pre_count;
	struct qcom_ipcc_wake_rec pre_first;
	u32 post_count;
	struct qcom_ipcc_wake_rec post[IPCC_WAKE_RECS];
};

struct qcom_ipcc_wake_trace {
	raw_spinlock_t lock;
	enum qcom_ipcc_wake_window window;
	u32 seq;
	struct qcom_ipcc_wake_cycle cycles[IPCC_WAKE_CYCLES];
	struct syscore syscore;
	struct dentry *debugfs;
};

/**
 * struct qcom_ipcc_chan_info - Per-mailbox-channel info
 * @client_id:	The client-id to which the interrupt has to be triggered
 * @signal_id:	The signal-id to which the interrupt has to be triggered
 */
struct qcom_ipcc_chan_info {
	u16 client_id;
	u16 signal_id;
};

/**
 * struct qcom_ipcc - Holder for the mailbox driver
 * @dev:		Device associated with this instance
 * @base:		Base address of the IPCC frame associated to APSS
 * @irq_domain:		The irq_domain associated with this instance
 * @chans:		The mailbox channels array
 * @mchan:		The per-mailbox channel info array
 * @mbox:		The mailbox controller
 * @num_chans:		Number of @chans elements
 * @irq:		Summary irq
 * @wake:		TEMP wake trace, NULL unless temp_wake_trace is set
 */
struct qcom_ipcc {
	struct device *dev;
	void __iomem *base;
	struct irq_domain *irq_domain;
	struct mbox_chan *chans;
	struct qcom_ipcc_chan_info *mchan;
	struct mbox_controller mbox;
	int num_chans;
	int irq;
	struct qcom_ipcc_wake_trace *wake;
};

static inline struct qcom_ipcc *to_qcom_ipcc(struct mbox_controller *mbox)
{
	return container_of(mbox, struct qcom_ipcc, mbox);
}

static inline u32 qcom_ipcc_get_hwirq(u16 client_id, u16 signal_id)
{
	return FIELD_PREP(IPCC_CLIENT_ID_MASK, client_id) |
	       FIELD_PREP(IPCC_SIGNAL_ID_MASK, signal_id);
}

static const char * const qcom_ipcc_client_names[] = {
	[IPCC_CLIENT_AOP] = "aop",
	[IPCC_CLIENT_TZ] = "tz",
	[IPCC_CLIENT_MPSS] = "mpss",
	[IPCC_CLIENT_LPASS] = "lpass",
	[IPCC_CLIENT_SLPI] = "slpi",
	[IPCC_CLIENT_SDC] = "sdc",
	[IPCC_CLIENT_CDSP] = "cdsp",
	[IPCC_CLIENT_NPU] = "npu",
	[IPCC_CLIENT_APSS] = "apss",
	[IPCC_CLIENT_GPU] = "gpu",
	[IPCC_CLIENT_CVP] = "cvp",
	[IPCC_CLIENT_CAM] = "cam",
	[IPCC_CLIENT_VPU] = "vpu",
	[IPCC_CLIENT_PCIE0] = "pcie0",
	[IPCC_CLIENT_PCIE1] = "pcie1",
	[IPCC_CLIENT_PCIE2] = "pcie2",
	[IPCC_CLIENT_SPSS] = "spss",
	[IPCC_CLIENT_NSP1] = "nsp1",
	[IPCC_CLIENT_TME] = "tme",
	[IPCC_CLIENT_WPSS] = "wpss",
	[IPCC_CLIENT_GPDSP0] = "gpdsp0",
	[IPCC_CLIENT_GPDSP1] = "gpdsp1",
};

static const char *qcom_ipcc_wake_client(u32 client)
{
	if (client < ARRAY_SIZE(qcom_ipcc_client_names) &&
	    qcom_ipcc_client_names[client])
		return qcom_ipcc_client_names[client];

	return "unknown";
}

/* Which protocol the signal belongs to, as wired in the SoC device trees */
static const char *qcom_ipcc_wake_source(u32 client, u32 signal)
{
	switch (signal) {
	case IPCC_MPROC_SIGNAL_GLINK_QMP:
		return client == IPCC_CLIENT_AOP ? "aoss-qmp" : "glink";
	case IPCC_MPROC_SIGNAL_TZ:
		return "tz";
	case IPCC_MPROC_SIGNAL_SMP2P:
		return "smp2p";
	case IPCC_MPROC_SIGNAL_PING:
		return "ping";
	default:
		return "other";
	}
}

static int qcom_ipcc_wake_desc(struct qcom_ipcc *ipcc, char *buf, size_t size,
			       u32 hwirq)
{
	u32 client = FIELD_GET(IPCC_CLIENT_ID_MASK, hwirq);
	u32 signal = FIELD_GET(IPCC_SIGNAL_ID_MASK, hwirq);

	if (hwirq == IPCC_NO_PENDING_IRQ)
		return scnprintf(buf, size, "client=none");
	if (hwirq == IPCC_WAKE_UNREAD)
		return scnprintf(buf, size, "client=unread");

	return scnprintf(buf, size,
			 "client=%u cname=%s signal=%u source=%s virq=%u",
			 client, qcom_ipcc_wake_client(client), signal,
			 qcom_ipcc_wake_source(client, signal),
			 irq_find_mapping(ipcc->irq_domain, hwirq));
}

static int qcom_ipcc_wake_point_line(struct qcom_ipcc *ipcc, char *buf,
				     size_t size,
				     const struct qcom_ipcc_wake_cycle *c,
				     enum qcom_ipcc_wake_point point)
{
	u32 rem;
	u64 sec = div_u64_rem(c->pending_ts[point], NSEC_PER_SEC, &rem);
	int n;

	n = scnprintf(buf, size, "seq=%u point=%s t=%llu.%06u ", c->seq,
		      qcom_ipcc_wake_point_names[point], sec,
		      (u32)(rem / NSEC_PER_USEC));

	return n + qcom_ipcc_wake_desc(ipcc, buf + n, size - n,
				       c->pending[point]);
}

static int qcom_ipcc_wake_rec_line(struct qcom_ipcc *ipcc, char *buf,
				   size_t size,
				   const struct qcom_ipcc_wake_cycle *c,
				   const char *window, unsigned int index,
				   const struct qcom_ipcc_wake_rec *rec)
{
	u32 rem;
	u64 sec = div_u64_rem(rec->ts, NSEC_PER_SEC, &rem);
	int n;

	n = scnprintf(buf, size, "seq=%u irq=%s index=%u t=%llu.%06u ",
		      c->seq, window, index, sec, (u32)(rem / NSEC_PER_USEC));
	n += qcom_ipcc_wake_desc(ipcc, buf + n, size - n, rec->hwirq);

	return n + scnprintf(buf + n, size - n, " disabled=%d", rec->disabled);
}

/*
 * Print one finished cycle, except the syscore resume point which has
 * already been printed as soon as it was read.
 */
static void qcom_ipcc_wake_print(struct qcom_ipcc *ipcc,
				 const struct qcom_ipcc_wake_cycle *c)
{
	char line[IPCC_WAKE_LINE];
	unsigned int i;

	for (i = 0; i < IPCC_WAKE_POINTS; i++) {
		if (!c->seen[i] || i == IPCC_WAKE_SYSCORE_RESUME)
			continue;
		qcom_ipcc_wake_point_line(ipcc, line, sizeof(line), c, i);
		dev_info(ipcc->dev, "TEMP wake: %s\n", line);
	}

	if (c->pre_count) {
		qcom_ipcc_wake_rec_line(ipcc, line, sizeof(line), c, "pre", 0,
					&c->pre_first);
		dev_info(ipcc->dev, "TEMP wake: %s\n", line);
	}

	for (i = 0; i < min_t(u32, c->post_count, IPCC_WAKE_RECS); i++) {
		qcom_ipcc_wake_rec_line(ipcc, line, sizeof(line), c, "post", i,
					&c->post[i]);
		dev_info(ipcc->dev, "TEMP wake: %s\n", line);
	}

	dev_info(ipcc->dev, "TEMP wake: seq=%u done pre_count=%u post_count=%u\n",
		 c->seq, c->pre_count, c->post_count);
}

/* Called from the summary interrupt handler, interrupts disabled */
static void qcom_ipcc_wake_note(struct qcom_ipcc *ipcc, u32 hwirq, int virq)
{
	struct qcom_ipcc_wake_trace *wake = ipcc->wake;
	struct qcom_ipcc_wake_rec *rec = NULL;
	struct qcom_ipcc_wake_cycle *c;
	struct irq_data *irqd;

	raw_spin_lock(&wake->lock);
	c = &wake->cycles[wake->seq % IPCC_WAKE_CYCLES];
	switch (wake->window) {
	case IPCC_WAKE_PRE:
		if (!c->pre_count++)
			rec = &c->pre_first;
		break;
	case IPCC_WAKE_POST:
		if (c->post_count < IPCC_WAKE_RECS)
			rec = &c->post[c->post_count];
		c->post_count++;
		break;
	default:
		break;
	}

	if (rec) {
		irqd = virq > 0 ? irq_get_irq_data(virq) : NULL;
		rec->ts = local_clock();
		rec->hwirq = hwirq;
		rec->virq = virq;
		rec->disabled = irqd && irqd_irq_disabled(irqd);
	}
	raw_spin_unlock(&wake->lock);
}

/*
 * Read the highest priority pending signal without consuming it: probe
 * cleared CLEAR_ON_RECV_RD, but do not trust that across a power collapse.
 */
static u32 qcom_ipcc_wake_peek(struct qcom_ipcc *ipcc)
{
	if (readl(ipcc->base + IPCC_REG_CONFIG) & IPCC_CLEAR_ON_RECV_RD)
		return IPCC_WAKE_UNREAD;

	return readl(ipcc->base + IPCC_REG_RECV_ID);
}

static void qcom_ipcc_wake_point(struct qcom_ipcc *ipcc,
				 enum qcom_ipcc_wake_point point,
				 enum qcom_ipcc_wake_window window)
{
	struct qcom_ipcc_wake_trace *wake = ipcc->wake;
	struct qcom_ipcc_wake_cycle *c;
	unsigned long flags;
	u32 hwirq;

	hwirq = qcom_ipcc_wake_peek(ipcc);

	raw_spin_lock_irqsave(&wake->lock, flags);
	if (point == IPCC_WAKE_NOIRQ_SUSPEND) {
		wake->seq++;
		c = &wake->cycles[wake->seq % IPCC_WAKE_CYCLES];
		memset(c, 0, sizeof(*c));
		c->seq = wake->seq;
	}
	c = &wake->cycles[wake->seq % IPCC_WAKE_CYCLES];
	c->seen[point] = true;
	c->pending[point] = hwirq;
	c->pending_ts[point] = local_clock();
	wake->window = window;
	raw_spin_unlock_irqrestore(&wake->lock, flags);
}

static int qcom_ipcc_wake_syscore_suspend(void *data)
{
	struct qcom_ipcc *ipcc = data;

	if (ipcc->wake->window == IPCC_WAKE_PRE)
		qcom_ipcc_wake_point(ipcc, IPCC_WAKE_SYSCORE_SUSPEND,
				     IPCC_WAKE_ASLEEP);

	return 0;
}

/*
 * Runs on the boot CPU with interrupts still disabled, right after the
 * firmware returns: whatever is pending here is what the summary handler
 * will take first once interrupts are enabled, before any noirq callback.
 */
static void qcom_ipcc_wake_syscore_resume(void *data)
{
	struct qcom_ipcc *ipcc = data;
	struct qcom_ipcc_wake_cycle *c;
	char line[IPCC_WAKE_LINE];

	if (ipcc->wake->window != IPCC_WAKE_ASLEEP)
		return;

	qcom_ipcc_wake_point(ipcc, IPCC_WAKE_SYSCORE_RESUME, IPCC_WAKE_POST);
	c = &ipcc->wake->cycles[ipcc->wake->seq % IPCC_WAKE_CYCLES];
	qcom_ipcc_wake_point_line(ipcc, line, sizeof(line), c,
				  IPCC_WAKE_SYSCORE_RESUME);
	dev_info(ipcc->dev, "TEMP wake: %s\n", line);
}

static const struct syscore_ops qcom_ipcc_wake_syscore_ops = {
	.suspend = qcom_ipcc_wake_syscore_suspend,
	.resume = qcom_ipcc_wake_syscore_resume,
};

static int qcom_ipcc_wake_show(struct seq_file *s, void *unused)
{
	struct qcom_ipcc *ipcc = s->private;
	struct qcom_ipcc_wake_trace *wake = ipcc->wake;
	struct qcom_ipcc_wake_cycle *cycles, *c;
	char line[IPCC_WAKE_LINE];
	unsigned long flags;
	u32 seq, first, i, j;

	cycles = kmalloc_objs(*cycles, IPCC_WAKE_CYCLES, GFP_KERNEL);
	if (!cycles)
		return -ENOMEM;

	raw_spin_lock_irqsave(&wake->lock, flags);
	memcpy(cycles, wake->cycles, sizeof(wake->cycles));
	seq = wake->seq;
	raw_spin_unlock_irqrestore(&wake->lock, flags);

	first = seq > IPCC_WAKE_CYCLES ? seq - IPCC_WAKE_CYCLES + 1 : 1;
	for (i = first; i <= seq; i++) {
		c = &cycles[i % IPCC_WAKE_CYCLES];
		for (j = 0; j < IPCC_WAKE_POINTS; j++) {
			if (!c->seen[j])
				continue;
			qcom_ipcc_wake_point_line(ipcc, line, sizeof(line), c, j);
			seq_printf(s, "%s\n", line);
		}
		if (c->pre_count) {
			qcom_ipcc_wake_rec_line(ipcc, line, sizeof(line), c,
						"pre", 0, &c->pre_first);
			seq_printf(s, "%s\n", line);
		}
		for (j = 0; j < min_t(u32, c->post_count, IPCC_WAKE_RECS); j++) {
			qcom_ipcc_wake_rec_line(ipcc, line, sizeof(line), c,
						"post", j, &c->post[j]);
			seq_printf(s, "%s\n", line);
		}
		seq_printf(s, "seq=%u done pre_count=%u post_count=%u\n",
			   c->seq, c->pre_count, c->post_count);
	}

	kfree(cycles);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qcom_ipcc_wake);

static int qcom_ipcc_wake_init(struct qcom_ipcc *ipcc)
{
	ipcc->wake = devm_kzalloc(ipcc->dev, sizeof(*ipcc->wake), GFP_KERNEL);
	if (!ipcc->wake)
		return -ENOMEM;

	raw_spin_lock_init(&ipcc->wake->lock);
	ipcc->wake->syscore.ops = &qcom_ipcc_wake_syscore_ops;
	ipcc->wake->syscore.data = ipcc;

	return 0;
}

static void qcom_ipcc_wake_start(struct qcom_ipcc *ipcc)
{
	char *name;

	register_syscore(&ipcc->wake->syscore);

	name = devm_kasprintf(ipcc->dev, GFP_KERNEL, "qcom_ipcc_wake_trace-%s",
			      dev_name(ipcc->dev));
	if (name)
		ipcc->wake->debugfs = debugfs_create_file(name, 0400, NULL, ipcc,
							  &qcom_ipcc_wake_fops);

	dev_info(ipcc->dev, "TEMP wake trace enabled\n");
}

static irqreturn_t qcom_ipcc_irq_fn(int irq, void *data)
{
	struct qcom_ipcc *ipcc = data;
	u32 hwirq;
	int virq;

	for (;;) {
		hwirq = readl(ipcc->base + IPCC_REG_RECV_ID);
		if (hwirq == IPCC_NO_PENDING_IRQ)
			break;

		virq = irq_find_mapping(ipcc->irq_domain, hwirq);
		if (unlikely(ipcc->wake))
			qcom_ipcc_wake_note(ipcc, hwirq, virq);
		writel(hwirq, ipcc->base + IPCC_REG_RECV_SIGNAL_CLEAR);
		generic_handle_irq(virq);
	}

	return IRQ_HANDLED;
}

static void qcom_ipcc_mask_irq(struct irq_data *irqd)
{
	struct qcom_ipcc *ipcc = irq_data_get_irq_chip_data(irqd);
	irq_hw_number_t hwirq = irqd_to_hwirq(irqd);

	writel(hwirq, ipcc->base + IPCC_REG_RECV_SIGNAL_DISABLE);
}

static void qcom_ipcc_unmask_irq(struct irq_data *irqd)
{
	struct qcom_ipcc *ipcc = irq_data_get_irq_chip_data(irqd);
	irq_hw_number_t hwirq = irqd_to_hwirq(irqd);

	writel(hwirq, ipcc->base + IPCC_REG_RECV_SIGNAL_ENABLE);
}

static struct irq_chip qcom_ipcc_irq_chip = {
	.name = "ipcc",
	.irq_mask = qcom_ipcc_mask_irq,
	.irq_unmask = qcom_ipcc_unmask_irq,
	.flags = IRQCHIP_SKIP_SET_WAKE,
};

static int qcom_ipcc_domain_map(struct irq_domain *d, unsigned int irq,
				irq_hw_number_t hw)
{
	struct qcom_ipcc *ipcc = d->host_data;

	irq_set_chip_and_handler(irq, &qcom_ipcc_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, ipcc);
	irq_set_noprobe(irq);

	return 0;
}

static int qcom_ipcc_domain_xlate(struct irq_domain *d,
				  struct device_node *node, const u32 *intspec,
				  unsigned int intsize,
				  unsigned long *out_hwirq,
				  unsigned int *out_type)
{
	if (intsize != 3)
		return -EINVAL;

	*out_hwirq = qcom_ipcc_get_hwirq(intspec[0], intspec[1]);
	*out_type = intspec[2] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static const struct irq_domain_ops qcom_ipcc_irq_ops = {
	.map = qcom_ipcc_domain_map,
	.xlate = qcom_ipcc_domain_xlate,
};

static int qcom_ipcc_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct qcom_ipcc *ipcc = to_qcom_ipcc(chan->mbox);
	struct qcom_ipcc_chan_info *mchan = chan->con_priv;
	u32 hwirq;

	hwirq = qcom_ipcc_get_hwirq(mchan->client_id, mchan->signal_id);
	writel(hwirq, ipcc->base + IPCC_REG_SEND_ID);

	return 0;
}

static void qcom_ipcc_mbox_shutdown(struct mbox_chan *chan)
{
	chan->con_priv = NULL;
}

static struct mbox_chan *qcom_ipcc_mbox_xlate(struct mbox_controller *mbox,
					const struct of_phandle_args *ph)
{
	struct qcom_ipcc *ipcc = to_qcom_ipcc(mbox);
	struct qcom_ipcc_chan_info *mchan;
	struct mbox_chan *chan;
	struct device *dev;
	int chan_id;

	dev = ipcc->dev;

	if (ph->args_count != 2)
		return ERR_PTR(-EINVAL);

	for (chan_id = 0; chan_id < mbox->num_chans; chan_id++) {
		chan = &ipcc->chans[chan_id];
		mchan = chan->con_priv;

		if (!mchan)
			break;
		else if (mchan->client_id == ph->args[0] &&
				mchan->signal_id == ph->args[1])
			return ERR_PTR(-EBUSY);
	}

	if (chan_id >= mbox->num_chans)
		return ERR_PTR(-EBUSY);

	mchan = devm_kzalloc(dev, sizeof(*mchan), GFP_KERNEL);
	if (!mchan)
		return ERR_PTR(-ENOMEM);

	mchan->client_id = ph->args[0];
	mchan->signal_id = ph->args[1];
	chan->con_priv = mchan;

	return chan;
}

static const struct mbox_chan_ops ipcc_mbox_chan_ops = {
	.send_data = qcom_ipcc_mbox_send_data,
	.shutdown = qcom_ipcc_mbox_shutdown,
};

static int qcom_ipcc_setup_mbox(struct qcom_ipcc *ipcc,
				struct device_node *controller_dn)
{
	struct of_phandle_args curr_ph;
	struct device_node *client_dn;
	struct mbox_controller *mbox;
	struct device *dev = ipcc->dev;
	int i, j, ret;

	/*
	 * Find out the number of clients interested in this mailbox
	 * and create channels accordingly.
	 */
	ipcc->num_chans = 0;
	for_each_node_with_property(client_dn, "mboxes") {
		if (!of_device_is_available(client_dn))
			continue;
		i = of_count_phandle_with_args(client_dn,
						"mboxes", "#mbox-cells");
		for (j = 0; j < i; j++) {
			ret = of_parse_phandle_with_args(client_dn, "mboxes",
						"#mbox-cells", j, &curr_ph);
			of_node_put(curr_ph.np);
			if (!ret && curr_ph.np == controller_dn)
				ipcc->num_chans++;
		}
	}

	/* If no clients are found, skip registering as a mbox controller */
	if (!ipcc->num_chans)
		return 0;

	ipcc->chans = devm_kcalloc(dev, ipcc->num_chans,
					sizeof(struct mbox_chan), GFP_KERNEL);
	if (!ipcc->chans)
		return -ENOMEM;

	mbox = &ipcc->mbox;
	mbox->dev = dev;
	mbox->num_chans = ipcc->num_chans;
	mbox->chans = ipcc->chans;
	mbox->ops = &ipcc_mbox_chan_ops;
	mbox->of_xlate = qcom_ipcc_mbox_xlate;
	mbox->txdone_irq = false;
	mbox->txdone_poll = false;

	return devm_mbox_controller_register(dev, mbox);
}

static int qcom_ipcc_pm_suspend(struct device *dev)
{
	struct qcom_ipcc *ipcc = dev_get_drvdata(dev);

	if (ipcc->wake && pm_suspend_in_progress())
		qcom_ipcc_wake_point(ipcc, IPCC_WAKE_NOIRQ_SUSPEND,
				     IPCC_WAKE_PRE);

	return 0;
}

static void qcom_ipcc_pm_complete(struct device *dev)
{
	struct qcom_ipcc *ipcc = dev_get_drvdata(dev);
	struct qcom_ipcc_wake_trace *wake = ipcc->wake;
	struct qcom_ipcc_wake_cycle c;
	unsigned long flags;

	if (!wake)
		return;

	raw_spin_lock_irqsave(&wake->lock, flags);
	if (wake->window == IPCC_WAKE_IDLE) {
		raw_spin_unlock_irqrestore(&wake->lock, flags);
		return;
	}
	wake->window = IPCC_WAKE_IDLE;
	c = wake->cycles[wake->seq % IPCC_WAKE_CYCLES];
	raw_spin_unlock_irqrestore(&wake->lock, flags);

	qcom_ipcc_wake_print(ipcc, &c);
}

static int qcom_ipcc_pm_resume(struct device *dev)
{
	struct qcom_ipcc *ipcc = dev_get_drvdata(dev);
	u32 hwirq;
	int virq;

	/* After s2idle, or a suspend that stopped short of syscore, the window opens here */
	if (ipcc->wake && ipcc->wake->window != IPCC_WAKE_IDLE)
		qcom_ipcc_wake_point(ipcc, IPCC_WAKE_NOIRQ_RESUME,
				     IPCC_WAKE_POST);

	hwirq = readl(ipcc->base + IPCC_REG_RECV_ID);
	if (hwirq == IPCC_NO_PENDING_IRQ)
		return 0;

	virq = irq_find_mapping(ipcc->irq_domain, hwirq);

	dev_dbg(dev, "virq: %d triggered client-id: %ld; signal-id: %ld\n", virq,
		FIELD_GET(IPCC_CLIENT_ID_MASK, hwirq), FIELD_GET(IPCC_SIGNAL_ID_MASK, hwirq));

	return 0;
}

static int qcom_ipcc_probe(struct platform_device *pdev)
{
	struct qcom_ipcc *ipcc;
	u32 config_value;
	static int id;
	char *name;
	int ret;

	ipcc = devm_kzalloc(&pdev->dev, sizeof(*ipcc), GFP_KERNEL);
	if (!ipcc)
		return -ENOMEM;

	ipcc->dev = &pdev->dev;

	ipcc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ipcc->base))
		return PTR_ERR(ipcc->base);

	/*
	 * It is possible that boot firmware is using the same IPCC instance
	 * as of the HLOS and it has kept CLEAR_ON_RECV_RD set which basically
	 * means Interrupt pending registers are cleared when RECV_ID is read.
	 * The register automatically updates to the next pending interrupt/client
	 * status based on priority.
	 */
	config_value = readl(ipcc->base + IPCC_REG_CONFIG);
	if (config_value & IPCC_CLEAR_ON_RECV_RD) {
		config_value &= ~(IPCC_CLEAR_ON_RECV_RD);
		writel(config_value, ipcc->base + IPCC_REG_CONFIG);
	}

	ipcc->irq = platform_get_irq(pdev, 0);
	if (ipcc->irq < 0)
		return ipcc->irq;

	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "ipcc_%d", id++);
	if (!name)
		return -ENOMEM;

	ipcc->irq_domain = irq_domain_create_tree(dev_fwnode(&pdev->dev), &qcom_ipcc_irq_ops, ipcc);
	if (!ipcc->irq_domain)
		return -ENOMEM;

	ret = qcom_ipcc_setup_mbox(ipcc, pdev->dev.of_node);
	if (ret)
		goto err_mbox;

	if (temp_wake_trace) {
		ret = qcom_ipcc_wake_init(ipcc);
		if (ret)
			goto err_req_irq;
	}

	ret = devm_request_irq(&pdev->dev, ipcc->irq, qcom_ipcc_irq_fn,
			       IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND |
			       IRQF_NO_THREAD, name, ipcc);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register the irq: %d\n", ret);
		goto err_req_irq;
	}

	platform_set_drvdata(pdev, ipcc);

	if (ipcc->wake)
		qcom_ipcc_wake_start(ipcc);

	return 0;

err_req_irq:
	if (ipcc->num_chans)
		mbox_controller_unregister(&ipcc->mbox);
err_mbox:
	irq_domain_remove(ipcc->irq_domain);

	return ret;
}

static void qcom_ipcc_remove(struct platform_device *pdev)
{
	struct qcom_ipcc *ipcc = platform_get_drvdata(pdev);

	if (ipcc->wake) {
		debugfs_remove(ipcc->wake->debugfs);
		unregister_syscore(&ipcc->wake->syscore);
	}
	disable_irq_wake(ipcc->irq);
	irq_domain_remove(ipcc->irq_domain);
}

static const struct of_device_id qcom_ipcc_of_match[] = {
	{ .compatible = "qcom,ipcc"},
	{}
};
MODULE_DEVICE_TABLE(of, qcom_ipcc_of_match);

static const struct dev_pm_ops qcom_ipcc_dev_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(qcom_ipcc_pm_suspend, qcom_ipcc_pm_resume)
	.complete = qcom_ipcc_pm_complete,
};

static struct platform_driver qcom_ipcc_driver = {
	.probe = qcom_ipcc_probe,
	.remove = qcom_ipcc_remove,
	.driver = {
		.name = "qcom-ipcc",
		.of_match_table = qcom_ipcc_of_match,
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&qcom_ipcc_dev_pm_ops),
	},
};

static int __init qcom_ipcc_init(void)
{
	return platform_driver_register(&qcom_ipcc_driver);
}
arch_initcall(qcom_ipcc_init);

MODULE_AUTHOR("Venkata Narendra Kumar Gutta <vnkgutta@codeaurora.org>");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. IPCC driver");
MODULE_LICENSE("GPL v2");
