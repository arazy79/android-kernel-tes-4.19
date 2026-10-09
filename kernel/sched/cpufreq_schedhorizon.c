/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include "sched.h"
#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>
#include <linux/sched/sysctl.h>
#include <linux/topology.h>
#include <linux/cpumask.h>

/* === TIDAK PERLU mask manual lagi — pakai cpu_core_mask bawaan kernel === */

static unsigned int default_efficient_freq_lp[] = {0};
static u64 default_up_delay_lp[] = {0};
static unsigned int default_efficient_freq_hp[] = {1958400};
static u64 default_up_delay_hp[] = {100 * NSEC_PER_MSEC};

struct sugov_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		up_rate_limit_us;
	unsigned int		down_rate_limit_us;
	unsigned int 		*efficient_freq;
	int 			nefficient_freq;
	u64 			*up_delay;
	int 			nup_delay;
	int 			current_step;
};

struct sugov_policy {
	struct cpufreq_policy	*policy;
	struct sugov_tunables	*tunables;
	struct list_head	tunables_hook;
	raw_spinlock_t		update_lock;
	u64			last_freq_update_time;
	s64			min_rate_limit_ns;
	s64			up_rate_delay_ns;
	s64			down_rate_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;
	unsigned int		prev_cached_raw_freq;
	u64	 		first_hp_request_time;
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;
	bool			limits_changed;
	bool			need_freq_update;
};

struct sugov_cpu {
	struct update_util_data	update_util;
	struct sugov_policy	*sg_policy;
	unsigned int		cpu;
	u64			last_update;
	struct sched_walt_cpu_load walt_load;
	unsigned long util;
	unsigned int flags;
	unsigned long		bw_dl;
	unsigned long		min;
	unsigned long		max;
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long		saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);
static unsigned int stale_ns;
static DEFINE_PER_CPU(struct sugov_tunables *, cached_tunables);

/************************ Governor internals ***********************/

static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;
	if (!cpufreq_this_cpu_can_update(sg_policy->policy))
		return false;
	if (unlikely(sg_policy->limits_changed)) {
		sg_policy->limits_changed = false;
		sg_policy->need_freq_update = true;
		return true;
	}
	delta_ns = time - sg_policy->last_freq_update_time;
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static inline bool use_pelt(void)
{
#ifdef CONFIG_SCHED_WALT
	return false;
#else
	return true;
#endif
}

static inline int match_nearest_efficient_step(int freq, int maxstep, int *freq_table)
{
	int i;
	for (i = 0; i < maxstep; i++) {
		if (freq_table[i] >= freq)
			break;
	}
	return i;
}

static inline void do_freq_limit(struct sugov_policy *sg_policy, unsigned int *freq, u64 time)
{
	if (*freq > sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step] && !sg_policy->first_hp_request_time) {
		*freq = sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step];
		sg_policy->first_hp_request_time = time;
	} else if (*freq < sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step]) {
		sg_policy->tunables->current_step = match_nearest_efficient_step(*freq, sg_policy->tunables->nefficient_freq, sg_policy->tunables->efficient_freq);
		sg_policy->first_hp_request_time = 0;
	} else if ((sg_policy->first_hp_request_time
		&& time < sg_policy->first_hp_request_time + sg_policy->tunables->up_delay[sg_policy->tunables->current_step])) {
		*freq = sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step];
	} else if (sg_policy->tunables->current_step + 1 <= sg_policy->tunables->nefficient_freq - 1
			&& sg_policy->tunables->current_step + 1 <= sg_policy->tunables->nup_delay - 1) {
		sg_policy->tunables->current_step++;
		sg_policy->first_hp_request_time = time;
		if (*freq > sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step])
			*freq = sg_policy->tunables->efficient_freq[sg_policy->tunables->current_step];
	}
}

static bool sugov_up_down_rate_limit(struct sugov_policy *sg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns;
	delta_ns = time - sg_policy->last_freq_update_time;
	if (next_freq > sg_policy->next_freq &&
	    delta_ns < sg_policy->up_rate_delay_ns)
			return true;
	if (next_freq < sg_policy->next_freq &&
	    delta_ns < sg_policy->down_rate_delay_ns)
			return true;
	return false;
}

static bool sugov_update_next_freq(struct sugov_policy *sg_policy, u64 time,
				   unsigned int next_freq)
{
	if (sg_policy->next_freq == next_freq)
		return false;
	if (sugov_up_down_rate_limit(sg_policy, time, next_freq)) {
		sg_policy->cached_raw_freq = sg_policy->prev_cached_raw_freq;
		return false;
	}
	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;
	return true;
}

static void sugov_fast_switch(struct sugov_policy *sg_policy, u64 time,
			      unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	if (!sugov_update_next_freq(sg_policy, time, next_freq))
		return;
	next_freq = cpufreq_driver_fast_switch(policy, next_freq);
	if (!next_freq)
		return;
	policy->cur = next_freq;
}

static void sugov_deferred_update(struct sugov_policy *sg_policy, u64 time,
				  unsigned int next_freq)
{
	if (!sugov_update_next_freq(sg_policy, time, next_freq))
		return;
	if (use_pelt())
		sg_policy->work_in_progress = true;
	irq_work_queue(&sg_policy->irq_work);
}

static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max, u64 time)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;
	freq = map_util_freq(util, freq, max);
	do_freq_limit(sg_policy, &freq, time);
	if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update)
		return sg_policy->next_freq;
	sg_policy->need_freq_update = false;
	sg_policy->prev_cached_raw_freq = sg_policy->cached_raw_freq;
	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

extern long
schedtune_cpu_margin_with(unsigned long util, int cpu, struct task_struct *p);

unsigned long schedhorizon_cpu_util(int cpu, unsigned long util_cfs,
				 unsigned long max, enum schedutil_type type,
				 struct task_struct *p)
{
	unsigned long dl_util, util, irq;
	struct rq *rq = cpu_rq(cpu);
	if (sched_feat(SUGOV_RT_MAX_FREQ) && !IS_BUILTIN(CONFIG_UCLAMP_TASK) &&
	    type == FREQUENCY_UTIL && rt_rq_is_runnable(&rq->rt)) {
		return max;
	}
	irq = cpu_util_irq(rq);
	if (unlikely(irq >= max))
		return max;
	util = util_cfs + cpu_util_rt(rq);
	if (type == FREQUENCY_UTIL)
#ifdef CONFIG_SCHED_TUNE
		util += schedtune_cpu_margin_with(util, cpu, p);
#else
		util = uclamp_rq_util_with(rq, util, p);
#endif
	dl_util = cpu_util_dl(rq);
	if (util + dl_util >= max)
		return max;
	if (type == ENERGY_UTIL)
		util += dl_util;
	util = scale_irq_capacity(util, irq, max);
	util += irq;
	if (type == FREQUENCY_UTIL)
		util += cpu_bw_dl(rq);
	return min(max, util);
}

#ifdef CONFIG_SCHED_WALT
static unsigned long sugov_get_util(struct sugov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);
	/* FIX 4.19: pakai arch_scale_cpu_capacity kalau topology_get_cpu_scale tidak ada */
	unsigned long max;
#ifdef topology_get_cpu_scale
	max = topology_get_cpu_scale(NULL, sg_cpu->cpu);
#else
	max = arch_scale_cpu_capacity(sg_cpu->cpu);
#endif
	sg_cpu->max = max;
	sg_cpu->bw_dl = cpu_bw_dl(rq);
	return stune_util(sg_cpu->cpu, 0, &sg_cpu->walt_load);
}
#else
static unsigned long sugov_get_util(struct sugov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);
	unsigned long util_cfs = cpu_util_cfs(rq);
	/* FIX 4.19: pakai arch_scale_cpu_capacity kalau topology_get_cpu_scale tidak ada */
	unsigned long max;
#ifdef topology_get_cpu_scale
	max = topology_get_cpu_scale(NULL, sg_cpu->cpu);
#else
	max = arch_scale_cpu_capacity(sg_cpu->cpu);
#endif
	sg_cpu->max = max;
	sg_cpu->bw_dl = cpu_bw_dl(rq);
	return schedhorizon_cpu_util(sg_cpu->cpu, util_cfs, max,
				  FREQUENCY_UTIL, NULL);
}
#endif

#ifdef CONFIG_NO_HZ_COMMON
static bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls_cpu(sg_cpu->cpu);
	bool ret = idle_calls == sg_cpu->saved_idle_calls;
	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu) { return false; }
#endif

static inline void ignore_dl_rate_limit(struct sugov_cpu *sg_cpu, struct sugov_policy *sg_policy)
{
	if (cpu_bw_dl(cpu_rq(sg_cpu->cpu)) > sg_cpu->bw_dl)
		sg_policy->limits_changed = true;
}

static void sugov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max;
	unsigned int next_f;
	bool busy;
	if (flags & SCHED_CPUFREQ_PL)
		return;
	sg_cpu->last_update = time;
	ignore_dl_rate_limit(sg_cpu, sg_policy);
	if (!sugov_should_update_freq(sg_policy, time))
		return;
	busy = use_pelt() && !sg_policy->need_freq_update &&
		sugov_cpu_is_busy(sg_cpu);
	sg_cpu->util = util = sugov_get_util(sg_cpu);
	max = sg_cpu->max;
	next_f = get_next_freq(sg_policy, util, max, time);
	if (busy && next_f < sg_policy->next_freq &&
	    sg_policy->next_freq != UINT_MAX) {
		next_f = sg_policy->next_freq;
		sg_policy->cached_raw_freq = sg_policy->prev_cached_raw_freq;
	}
	if (sg_policy->policy->fast_switch_enabled) {
		sugov_fast_switch(sg_policy, time, next_f);
	} else {
		raw_spin_lock(&sg_policy->update_lock);
		sugov_deferred_update(sg_policy, time, next_f);
		raw_spin_unlock(&sg_policy->update_lock);
	}
}

static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu, u64 time)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;
	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu = &per_cpu(sugov_cpu, j);
		unsigned long j_util, j_max;
		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		if (j_util * max > j_max * util) {
			util = j_util;
			max = j_max;
		}
	}
	return get_next_freq(sg_policy, util, max, time);
}

static void
sugov_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int next_f;
	if (flags & SCHED_CPUFREQ_PL)
		return;
	sg_cpu->util = sugov_get_util(sg_cpu);
	sg_cpu->flags = flags;
	raw_spin_lock(&sg_policy->update_lock);
	sg_cpu->last_update = time;
	ignore_dl_rate_limit(sg_cpu, sg_policy);
	if (sugov_should_update_freq(sg_policy, time) &&
	    !(flags & SCHED_CPUFREQ_CONTINUE)) {
		next_f = sugov_next_freq_shared(sg_cpu, time);
		if (sg_policy->policy->fast_switch_enabled)
			sugov_fast_switch(sg_policy, time, next_f);
		else
			sugov_deferred_update(sg_policy, time, next_f);
	}
	raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_work(struct kthread_work *work)
{
	struct sugov_policy *sg_policy = container_of(work, struct sugov_policy, work);
	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, sg_policy->next_freq,
				CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
}

static void sugov_irq_work(struct irq_work *irq_work)
{
	struct sugov_policy *sg_policy;
	sg_policy = container_of(irq_work, struct sugov_policy, irq_work);
	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

static unsigned int *resolve_data_freq(const char *buf, int *num_ret, size_t count)
{
	const char *cp;
	unsigned int *output;
	int num = 1, i;
	cp = buf;
	while ((cp = strpbrk(cp + 1, " ")))
		num++;
	output = kmalloc(num * sizeof(unsigned int), GFP_KERNEL);
	if (!output)
		return NULL;
	cp = buf;
	i = 0;
	while (i < num && cp - buf < count) {
		if (sscanf(cp, "%u", &output[i++]) != 1)
			goto err_kfree;
		cp = strpbrk(cp, " ");
		if (!cp)
			break;
		cp++;
	}
	*num_ret = num;
	return output;
err_kfree:
	kfree(output);
	return NULL;
}

static u64 *resolve_data_delay(const char *buf, int *num_ret, size_t count)
{
	const char *cp;
	u64 *output;
	int num = 1, i;
	cp = buf;
	while ((cp = strpbrk(cp + 1, " ")))
		num++;
	output = kzalloc(num * sizeof(u64), GFP_KERNEL);
	if (!output)
		return NULL;
	cp = buf;
	i = 0;
	while (i < num && cp - buf < count) {
		if (sscanf(cp, "%llu", &output[i]) == 1) {
			output[i] = output[i] * NSEC_PER_MSEC;
			i++;
		} else {
			goto err_kfree;
		}
		cp = strpbrk(cp, " ");
		if (!cp)
			break;
		cp++;
	}
	*num_ret = num;
	return output;
err_kfree:
	kfree(output);
	return NULL;
}

/************************** sysfs interface ************************/
static struct sugov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);

static void update_min_rate_limit_ns(struct sugov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;
	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;
	tunables->up_rate_limit_us = rate_limit_us;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_ns(sg_policy);
	}
	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;
	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;
	tunables->down_rate_limit_us = rate_limit_us;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_ns(sg_policy);
	}
	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);

static ssize_t efficient_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	int i;
	ssize_t ret = 0;
	for (i = 0; i < tunables->nefficient_freq; i++)
		ret += sprintf(buf + ret, "%u ", tunables->efficient_freq[i]);
	sprintf(buf + ret - 1, "\n");
	return ret;
}

static ssize_t up_delay_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	int i;
	ssize_t ret = 0;
	for (i = 0; i < tunables->nup_delay; i++)
		ret += sprintf(buf + ret, "%u ", tunables->up_delay[i] / NSEC_PER_MSEC);
	sprintf(buf + ret - 1, "\n");
	return ret;
}

static ssize_t efficient_freq_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	int new_num;
	unsigned int *new_efficient_freq = NULL, *old;
	new_efficient_freq = resolve_data_freq(buf, &new_num, count);
	if (new_efficient_freq) {
	    old = tunables->efficient_freq;
	    tunables->efficient_freq = new_efficient_freq;
	    tunables->nefficient_freq = new_num;
	    tunables->current_step = 0;
	    if (old != default_efficient_freq_lp && old != default_efficient_freq_hp)
	        kfree(old);
	}
	return count;
}

static ssize_t up_delay_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	int new_num;
	u64 *new_up_delay = NULL, *old;
	new_up_delay = resolve_data_delay(buf, &new_num, count);
	if (new_up_delay) {
	    old = tunables->up_delay;
	    tunables->up_delay = new_up_delay;
	    tunables->nup_delay = new_num;
	    tunables->current_step = 0;
	    if (old != default_up_delay_lp && old != default_up_delay_hp)
	        kfree(old);
	}
	return count;
}

static struct governor_attr efficient_freq = __ATTR_RW(efficient_freq);
static struct governor_attr up_delay = __ATTR_RW(up_delay);

static struct attribute *sugov_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&efficient_freq.attr,
	&up_delay.attr,
	NULL
};

static void sugov_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);
	kfree(to_sugov_tunables(attr_set));
}

static struct kobj_type sugov_tunables_ktype = {
	.default_attrs = sugov_attributes,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sugov_tunables_free,
};

/********************** cpufreq governor interface *********************/
static struct cpufreq_governor schedhorizon_gov;

static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;
	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static inline void sugov_policy_free(struct sugov_policy *sg_policy)
{
	kfree(sg_policy);
}

static int sugov_kthread_create(struct sugov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;
	if (policy->fast_switch_enabled)
		return 0;
	kthread_init_work(&sg_policy->work, sugov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}
	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}
	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	mutex_init(&sg_policy->work_lock);
	wake_up_process(thread);
	return 0;
}

static void sugov_kthread_stop(struct sugov_policy *sg_policy)
{
	if (sg_policy->policy->fast_switch_enabled)
		return;
	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;
	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void sugov_tunables_save(struct cpufreq_policy *policy,
		struct sugov_tunables *tunables)
{
	int cpu;
	struct sugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);
	if (!have_governor_per_policy())
		return;
	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;
		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}
	cached->up_rate_limit_us = tunables->up_rate_limit_us;
	cached->down_rate_limit_us = tunables->down_rate_limit_us;
	cached->efficient_freq = tunables->efficient_freq;
	cached->up_delay = tunables->up_delay;
	cached->nefficient_freq = tunables->nefficient_freq;
	cached->nup_delay = tunables->nup_delay;
}

static void sugov_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
}

static void sugov_tunables_restore(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	struct sugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);
	if (!cached)
		return;
	tunables->up_rate_limit_us = cached->up_rate_limit_us;
	tunables->down_rate_limit_us = cached->down_rate_limit_us;
	tunables->efficient_freq = cached->efficient_freq;
	tunables->up_delay = cached->up_delay;
	tunables->nefficient_freq = cached->nefficient_freq;
	tunables->nup_delay = cached->nup_delay;
}

static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	int ret = 0;
	if (policy->governor_data)
		return -EBUSY;
	cpufreq_enable_fast_switch(policy);
	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}
	ret = sugov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;
	mutex_lock(&global_tunables_lock);
	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;
		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}
	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}
	tunables->up_rate_limit_us = cpufreq_policy_transition_delay_us(policy);
	tunables->down_rate_limit_us = cpufreq_policy_transition_delay_us(policy);

	/* === PAKAI MASK BAWAAN KERNEL — aman untuk SDM660 4+4 === */
	if (cpumask_test_cpu(sg_policy->policy->cpu, cpu_core_mask(0))) {
		/* Cluster 0 = CPU 0-3 = Little / LP */
		tunables->efficient_freq = default_efficient_freq_lp;
    		tunables->nefficient_freq = ARRAY_SIZE(default_efficient_freq_lp);
		tunables->up_delay = default_up_delay_lp;
		tunables->nup_delay = ARRAY_SIZE(default_up_delay_lp);
	} else {
		/* Cluster 1 = CPU 4-7 = Big / Perf */
		tunables->efficient_freq = default_efficient_freq_hp;
    		tunables->nefficient_freq = ARRAY_SIZE(default_efficient_freq_hp);
		tunables->up_delay = default_up_delay_hp;
		tunables->nup_delay = ARRAY_SIZE(default_up_delay_hp);
	}

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;
	stale_ns = sched_ravg_window + (sched_ravg_window >> 3);
	sugov_tunables_restore(policy);
	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   schedhorizon_gov.name);
	if (ret)
		goto fail;
out:
	mutex_unlock(&global_tunables_lock);
	return 0;
fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	sugov_clear_global_tunables();
stop_kthread:
	sugov_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);
free_sg_policy:
	sugov_policy_free(sg_policy);
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;
	mutex_lock(&global_tunables_lock);
	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		sugov_tunables_save(policy, tunables);
		sugov_clear_global_tunables();
	}
	mutex_unlock(&global_tunables_lock);
	sugov_kthread_stop(sg_policy);
	sugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;
	sg_policy->up_rate_delay_ns =
		sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	update_min_rate_limit_ns(sg_policy);
	sg_policy->last_freq_update_time	= 0;
	sg_policy->next_freq			= 0;
	sg_policy->work_in_progress		= false;
	sg_policy->limits_changed		= false;
	sg_policy->need_freq_update		= false;
	sg_policy->cached_raw_freq		= 0;
	sg_policy->prev_cached_raw_freq		= 0;
	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);
		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu			= cpu;
		sg_cpu->sg_policy		= sg_policy;
		sg_cpu->min			=
			(SCHED_CAPACITY_SCALE * policy->cpuinfo.min_freq) /
			policy->cpuinfo.max_freq;
	}
	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);
		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
					     policy_is_shared(policy) ?
							sugov_update_shared :
							sugov_update_single);
	}
	return 0;
}

static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;
	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);
	synchronize_sched();
	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned long flags, now;
	unsigned int freq;
	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	} else {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		freq = policy->cur;
		now = ktime_get_ns();
		freq = cpufreq_driver_resolve_freq(policy, freq);
		sg_policy->cached_raw_freq = freq;
		sugov_fast_switch(sg_policy, now, freq);
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}
	sg_policy->limits_changed = true;
}

static struct cpufreq_governor schedhorizon_gov = {
	.name			= "schedhorizon",
	.owner			= THIS_MODULE,
	.dynamic_switching	= true,
	.init			= sugov_init,
	.exit			= sugov_exit,
	.start			= sugov_start,
	.stop			= sugov_stop,
	.limits			= sugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDHORIZON
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedhorizon_gov;
}
#endif

static int __init schedhorizon_gov_init(void)
{
	/* TIDAK PERLU panggil schedhorizon_init_masks() — sudah pakai mask bawaan kernel */
	return cpufreq_register_governor(&schedhorizon_gov);
}

static void __exit schedhorizon_gov_exit(void)
{
	cpufreq_unregister_governor(&schedhorizon_gov);
}

module_init(schedhorizon_gov_init);
module_exit(schedhorizon_gov_exit);
