/* flow.c: Generic flow cache.
 *
 * Copyright (C) 2003 Alexey N. Kuznetsov (kuznet@ms2.inr.ac.ru)
 * Copyright (C) 2003 David S. Miller (davem@redhat.com)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/jhash.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/completion.h>
#include <linux/percpu.h>
#include <linux/bitops.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/mutex.h>
#include <net/flow.h>
#include <asm/atomic.h>
#include <linux/security.h>

struct flow_cache_entry {
	struct flow_cache_entry	*next;
	u16			family;
	u8			dir;
	u32			genid;
	struct flowi		key;
	void			*object;
	atomic_t		*object_ref;
};

struct flow_cache_percpu {
	struct flow_cache_entry **	hash_table;
	int				hash_count;
	u32				hash_rnd;
	int				hash_rnd_recalc;
	struct tasklet_struct		flush_tasklet;
};

struct flow_flush_info {
	struct flow_cache *		cache;
	atomic_t			cpuleft;
	struct completion		completion;
};

struct flow_cache {
	u32				hash_shift;
	unsigned long			order;
	struct flow_cache_percpu *	percpu;
	struct notifier_block		hotcpu_notifier;
	int				low_watermark;
	int				high_watermark;
	struct timer_list		rnd_timer;
};

atomic_t flow_cache_genid = ATOMIC_INIT(0);
static struct flow_cache flow_cache_global;
static struct kmem_cache *flow_cachep;

#define flow_cache_hash_size(cache)	(1 << (cache)->hash_shift)
#define FLOW_HASH_RND_PERIOD		(10 * 60 * HZ)

static void flow_cache_new_hashrnd(unsigned long arg)
{
	struct flow_cache *fc = (void *) arg;
	int i;

	for_each_possible_cpu(i)
		per_cpu_ptr(fc->percpu, i)->hash_rnd_recalc = 1;

	fc->rnd_timer.expires = jiffies + FLOW_HASH_RND_PERIOD;
	add_timer(&fc->rnd_timer);
}

static void flow_entry_kill(struct flow_cache *fc,
			    struct flow_cache_percpu *fcp,
			    struct flow_cache_entry *fle)
{
	if (fle->object)
		atomic_dec(fle->object_ref);
	kmem_cache_free(flow_cachep, fle);
	fcp->hash_count--;
}

static void __flow_cache_shrink(struct flow_cache *fc,
				struct flow_cache_percpu *fcp,
				int shrink_to)
{
	struct flow_cache_entry *fle, **flp;
	int i;

	for (i = 0; i < flow_cache_hash_size(fc); i++) {
		int k = 0;

		flp = &fcp->hash_table[i];
		while ((fle = *flp) != NULL && k < shrink_to) {
			k++;
			flp = &fle->next;
		}
		while ((fle = *flp) != NULL) {
			*flp = fle->next;
			flow_entry_kill(fc, fcp, fle);
		}
	}
}

static void flow_cache_shrink(struct flow_cache *fc,
			      struct flow_cache_percpu *fcp)
{
	int shrink_to = fc->low_watermark / flow_cache_hash_size(fc);

	__flow_cache_shrink(fc, fcp, shrink_to);
}

static void flow_new_hash_rnd(struct flow_cache *fc,
			      struct flow_cache_percpu *fcp)
{
	get_random_bytes(&fcp->hash_rnd, sizeof(u32));
	fcp->hash_rnd_recalc = 0;
	__flow_cache_shrink(fc, fcp, 0);
}

static u32 flow_hash_code(struct flow_cache *fc,
			  struct flow_cache_percpu *fcp,
			  struct flowi *key)
{
	u32 *k = (u32 *) key;

	return (jhash2(k, (sizeof(*key) / sizeof(u32)), fcp->hash_rnd)
		& (flow_cache_hash_size(fc) - 1));
}

#if (BITS_PER_LONG == 64)
typedef u64 flow_compare_t;
#else
typedef u32 flow_compare_t;
#endif

/* I hear what you're saying, use memcmp.  But memcmp cannot make
 * important assumptions that we can here, such as alignment and
 * constant size.
 */
static int flow_key_compare(struct flowi *key1, struct flowi *key2)
{
	flow_compare_t *k1, *k1_lim, *k2;
	const int n_elem = sizeof(struct flowi) / sizeof(flow_compare_t);

	BUILD_BUG_ON(sizeof(struct flowi) % sizeof(flow_compare_t));

	k1 = (flow_compare_t *) key1;
	k1_lim = k1 + n_elem;

	k2 = (flow_compare_t *) key2;

	do {
		if (*k1++ != *k2++)
			return 1;
	} while (k1 < k1_lim);

	return 0;
}

void *flow_cache_lookup(struct net *net, struct flowi *key, u16 family, u8 dir,
			flow_resolve_t resolver)
{
	struct flow_cache *fc = &flow_cache_global;
	struct flow_cache_percpu *fcp;
	struct flow_cache_entry *fle, **head;
	unsigned int hash;

	local_bh_disable();
	fcp = per_cpu_ptr(fc->percpu, smp_processor_id());

	fle = NULL;
	/* Packet really early in init?  Making flow_cache_init a
	 * pre-smp initcall would solve this.  --RR */
	if (!fcp->hash_table)
		goto nocache;

	if (fcp->hash_rnd_recalc)
		flow_new_hash_rnd(fc, fcp);
	hash = flow_hash_code(fc, fcp, key);

	head = &fcp->hash_table[hash];
	for (fle = *head; fle; fle = fle->next) {
		if (fle->family == family &&
		    fle->dir == dir &&
		    flow_key_compare(key, &fle->key) == 0) {
			if (fle->genid == atomic_read(&flow_cache_genid)) {
				void *ret = fle->object;

				if (ret)
					atomic_inc(fle->object_ref);
				local_bh_enable();

				return ret;
			}
			break;
		}
	}

	if (!fle) {
		if (fcp->hash_count > fc->high_watermark)
			flow_cache_shrink(fc, fcp);

		fle = kmem_cache_alloc(flow_cachep, GFP_ATOMIC);
		if (fle) {
			fle->next = *head;
			*head = fle;
			fle->family = family;
			fle->dir = dir;
			memcpy(&fle->key, key, sizeof(*key));
			fle->object = NULL;
			fcp->hash_count++;
		}
	}

nocache:
	{
		int err;
		void *obj;
		atomic_t *obj_ref;

		err = resolver(net, key, family, dir, &obj, &obj_ref);

		if (fle && !err) {
			fle->genid = atomic_read(&flow_cache_genid);

			if (fle->object)
				atomic_dec(fle->object_ref);

			fle->object = obj;
			fle->object_ref = obj_ref;
			if (obj)
				atomic_inc(fle->object_ref);
		}
		local_bh_enable();

		if (err)
			obj = ERR_PTR(err);
		return obj;
	}
}

static void flow_cache_flush_tasklet(unsigned long data)
{
	struct flow_flush_info *info = (void *)data;
	struct flow_cache *fc = info->cache;
	struct flow_cache_percpu *fcp;
	int i;

	fcp = per_cpu_ptr(fc->percpu, smp_processor_id());
	for (i = 0; i < flow_cache_hash_size(fc); i++) {
		struct flow_cache_entry *fle;

		fle = fcp->hash_table[i];
		for (; fle; fle = fle->next) {
			unsigned genid = atomic_read(&flow_cache_genid);

			if (!fle->object || fle->genid == genid)
				continue;

			fle->object = NULL;
			atomic_dec(fle->object_ref);
		}
	}

	if (atomic_dec_and_test(&info->cpuleft))
		complete(&info->completion);
}

static void flow_cache_flush_per_cpu(void *data)
{
	struct flow_flush_info *info = data;
	int cpu;
	struct tasklet_struct *tasklet;

	cpu = smp_processor_id();
	tasklet = &per_cpu_ptr(info->cache->percpu, cpu)->flush_tasklet;
	tasklet->data = (unsigned long)info;
	tasklet_schedule(tasklet);
}

void flow_cache_flush(void)
{
	struct flow_flush_info info;
	static DEFINE_MUTEX(flow_flush_sem);

	/* Don't want cpus going down or up during this. */
	get_online_cpus();
	mutex_lock(&flow_flush_sem);
	info.cache = &flow_cache_global;
	atomic_set(&info.cpuleft, num_online_cpus());
	init_completion(&info.completion);

	local_bh_disable();
	smp_call_function(flow_cache_flush_per_cpu, &info, 0);
	flow_cache_flush_tasklet((unsigned long)&info);
	local_bh_enable();

	wait_for_completion(&info.completion);
	mutex_unlock(&flow_flush_sem);
	put_online_cpus();
}

static void __init flow_cache_cpu_prepare(struct flow_cache *fc,
					  struct flow_cache_percpu *fcp)
{
	fcp->hash_table = (struct flow_cache_entry **)
		__get_free_pages(GFP_KERNEL|__GFP_ZERO, fc->order);
	if (!fcp->hash_table)
		panic("NET: failed to allocate flow cache order %lu\n", fc->order);

	fcp->hash_rnd_recalc = 1;
	fcp->hash_count = 0;
	tasklet_init(&fcp->flush_tasklet, flow_cache_flush_tasklet, 0);
}

static int flow_cache_cpu(struct notifier_block *nfb,
			  unsigned long action,
			  void *hcpu)
{
	struct flow_cache *fc = container_of(nfb, struct flow_cache, hotcpu_notifier);
	int cpu = (unsigned long) hcpu;
	struct flow_cache_percpu *fcp = per_cpu_ptr(fc->percpu, cpu);

	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN)
		__flow_cache_shrink(fc, fcp, 0);
	return NOTIFY_OK;
}

static int flow_cache_init(struct flow_cache *fc)
{
	unsigned long order;
	int i;

	fc->hash_shift = 10;
	fc->low_watermark = 2 * flow_cache_hash_size(fc);
	fc->high_watermark = 4 * flow_cache_hash_size(fc);

	for (order = 0;
	     (PAGE_SIZE << order) <
		     (sizeof(struct flow_cache_entry *)*flow_cache_hash_size(fc));
	     order++)
		/* NOTHING */;
	fc->order = order;
	fc->percpu = alloc_percpu(struct flow_cache_percpu);

	setup_timer(&fc->rnd_timer, flow_cache_new_hashrnd,
		    (unsigned long) fc);
	fc->rnd_timer.expires = jiffies + FLOW_HASH_RND_PERIOD;
	add_timer(&fc->rnd_timer);

	for_each_possible_cpu(i)
		flow_cache_cpu_prepare(fc, per_cpu_ptr(fc->percpu, i));

	fc->hotcpu_notifier = (struct notifier_block){
		.notifier_call = flow_cache_cpu,
	};
	register_hotcpu_notifier(&fc->hotcpu_notifier);

	return 0;
}

static int __init flow_cache_init_global(void)
{
	flow_cachep = kmem_cache_create("flow_cache",
					sizeof(struct flow_cache_entry),
					0, SLAB_PANIC, NULL);

	return flow_cache_init(&flow_cache_global);
}

module_init(flow_cache_init_global);

EXPORT_SYMBOL(flow_cache_genid);
EXPORT_SYMBOL(flow_cache_lookup);
