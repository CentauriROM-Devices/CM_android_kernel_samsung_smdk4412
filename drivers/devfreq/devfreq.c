/*
 * devfreq: Generic Dynamic Voltage and Frequency Scaling (DVFS) Framework
 *	    for Non-CPU Devices.
 *
 * Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/opp.h>
#include <linux/devfreq.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/hrtimer.h>
#include <linux/pm_qos_params.h>
#include "governor.h"

struct class *devfreq_class;

/*
 * devfreq_work periodically monitors every registered device.
 * The minimum polling interval is one jiffy. The polling interval is
 * determined by the minimum polling period among all polling devfreq
 * devices. The resolution of polling interval is one jiffy.
 */
static bool polling;
static struct workqueue_struct *devfreq_wq;
static struct delayed_work devfreq_work;

/* wait removing if this is to be removed */
static struct devfreq *wait_remove_device;

/* The list of all device-devfreq */
static LIST_HEAD(devfreq_list);
static DEFINE_MUTEX(devfreq_list_lock);

/**
 * find_device_devfreq() - find devfreq struct using device pointer
 * @dev:	device pointer used to lookup device devfreq.
 *
 * Search the list of device devfreqs and return the matched device's
 * devfreq info. devfreq_list_lock should be held by the caller.
 */
static struct devfreq *find_device_devfreq(struct device *dev)
{
	struct devfreq *tmp_devfreq;

	if (unlikely(IS_ERR_OR_NULL(dev))) {
		pr_err("DEVFREQ: %s: Invalid parameters\n", __func__);
		return ERR_PTR(-EINVAL);
	}
	WARN(!mutex_is_locked(&devfreq_list_lock),
	     "devfreq_list_lock must be locked.");

	list_for_each_entry(tmp_devfreq, &devfreq_list, node) {
		if (tmp_devfreq->dev.parent == dev)
			return tmp_devfreq;
	}

	return ERR_PTR(-ENODEV);
}

/**
 * update_devfreq() - Reevaluate the device and configure frequency.
 * @devfreq:	the devfreq instance.
 *
 * Note: Lock devfreq->lock before calling update_devfreq
 *	 This function is exported for governors.
 */
int update_devfreq(struct devfreq *devfreq)
{
	unsigned long freq;
	int err = 0;
	u32 options = 0;

	if (!mutex_is_locked(&devfreq->lock)) {
		WARN(true, "devfreq->lock must be locked by the caller.\n");
		return -EINVAL;
	}

	/* Reevaluate the proper frequency */
	err = devfreq->governor->get_target_freq(devfreq, &freq);
	if (err)
		return err;

	/*
	 * Adjust the freuqency with user freq and QoS.
	 *
	 * List from the highest proiority
	 * min_freq
	 * max_freq
	 * qos_min_freq
	 */

	if (devfreq->qos_min_freq && freq < devfreq->qos_min_freq) {
		freq = devfreq->qos_min_freq;
		options &= ~(1 << 0);
		options |= DEVFREQ_OPTION_FREQ_LUB;
	}
	if (devfreq->max_freq && freq > devfreq->max_freq) {
		freq = devfreq->max_freq;
		options &= ~(1 << 0);
		options |= DEVFREQ_OPTION_FREQ_GLB;
	}
	if (devfreq->min_freq && freq < devfreq->min_freq) {
		freq = devfreq->min_freq;
		options &= ~(1 << 0);
		options |= DEVFREQ_OPTION_FREQ_LUB;
	}

	err = devfreq->profile->target(devfreq->dev.parent, &freq, options);
	if (err)
		return err;

	devfreq->previous_freq = freq;
	return err;
}

/**
 * devfreq_notifier_call() - Notify that the device frequency requirements
 *			   has been changed out of devfreq framework.
 * @nb		the notifier_block (supposed to be devfreq->nb)
 * @val		not used.
 * @devp	not used
 *
 * Called by a notifier that uses devfreq->nb.
 */
static int devfreq_notifier_call(struct notifier_block *nb, unsigned long val,
				 void *devp)
{
	struct devfreq *devfreq = container_of(nb, struct devfreq, nb);
	int ret;

	mutex_lock(&devfreq->lock);
	ret = update_devfreq(devfreq);
	mutex_unlock(&devfreq->lock);

	return ret;
}

/**
 * devfreq_qos_notifier_call() -
 */
static int devfreq_qos_notifier_call(struct notifier_block *nb,
				     unsigned long value, void *devp)
{
	struct devfreq *devfreq = container_of(nb, struct devfreq, qos_nb);
	int ret;
	int i;
	unsigned long default_value = 0;
	struct devfreq_pm_qos_table *qos_list = devfreq->profile->qos_list;
	bool qos_use_max = devfreq->profile->qos_use_max;

	if (qos_list)
		return NOTIFY_DONE;

	/* TODO: */
	mutex_lock(&devfreq->lock);

	switch (devfreq->profile->qos_type) {
	case PM_QOS_CPU_DMA_LATENCY:
		default_value = PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE;
		break;
	case PM_QOS_NETWORK_LATENCY:
		default_value = PM_QOS_NETWORK_LAT_DEFAULT_VALUE;
		break;
	case PM_QOS_NETWORK_THROUGHPUT:
		default_value = PM_QOS_NETWORK_THROUGHPUT_DEFAULT_VALUE;
		break;
	case PM_QOS_BUS_DMA_THROUGHPUT:
		default_value = PM_QOS_BUS_DMA_THROUGHPUT_DEFAULT_VALUE;
		break;
	default:
		/* Won't do any check to detect "default" state */
		break;
	}

	if (value == default_value) {
		devfreq->qos_min_freq = 0;
		goto update;
	}

	for (i = 0; qos_list[i].freq; i++) {
		/* Qos Met */
		if ((qos_use_max && qos_list[i].qos_value <= value) ||
		    (!qos_use_max && qos_list[i].qos_value >= value)) {
			devfreq->qos_min_freq = qos_list[i].freq;
			goto update;
		}
	}

	/* Use the highest QoS freq */
	if (i > 0)
		devfreq->qos_min_freq = qos_list[i - 1].freq;

update:
	ret = update_devfreq(devfreq);
	mutex_unlock(&devfreq->lock);

	return ret;
}

/**
 * _remove_devfreq() - Remove devfreq from the device.
 * @devfreq:	the devfreq struct
 * @skip:	skip calling device_unregister().
 *
 * Note that the caller should lock devfreq->lock before calling
 * this. _remove_devfreq() will unlock it and free devfreq
 * internally. devfreq_list_lock should be locked by the caller
 * as well (not relased at return)
 *
 * Lock usage:
 * devfreq->lock: locked before call.
 *		  unlocked at return (and freed)
 * devfreq_list_lock: locked before call.
 *		      kept locked at return.
 *		      if devfreq is centrally polled.
 *
 * Freed memory:
 * devfreq
 */
static void _remove_devfreq(struct devfreq *devfreq, bool skip)
{
	if (!mutex_is_locked(&devfreq->lock)) {
		WARN(true, "devfreq->lock must be locked by the caller.\n");
		return;
	}
	if (!devfreq->governor->no_central_polling &&
	    !mutex_is_locked(&devfreq_list_lock)) {
		WARN(true, "devfreq_list_lock must be locked by the caller.\n");
		return;
	}

	if (devfreq->being_removed)
		return;

	devfreq->being_removed = true;

	if (devfreq->profile->qos_type)
		pm_qos_remove_notifier(devfreq->profile->qos_type,
				       &devfreq->qos_nb);

	if (devfreq->profile->exit)
		devfreq->profile->exit(devfreq->dev.parent);

	if (devfreq->governor->exit)
		devfreq->governor->exit(devfreq);

	if (!skip && get_device(&devfreq->dev)) {
		device_unregister(&devfreq->dev);
		put_device(&devfreq->dev);
	}

	if (!devfreq->governor->no_central_polling)
		list_del(&devfreq->node);

	mutex_unlock(&devfreq->lock);
	mutex_destroy(&devfreq->lock);

	kfree(devfreq);
}

/**
 * devfreq_dev_release() - Callback for struct device to release the device.
 * @dev:	the devfreq device
 *
 * This calls _remove_devfreq() if _remove_devfreq() is not called.
 * Note that devfreq_dev_release() could be called by _remove_devfreq() as
 * well as by others unregistering the device.
 */
static void devfreq_dev_release(struct device *dev)
{
	struct devfreq *devfreq = to_devfreq(dev);
	bool central_polling = !devfreq->governor->no_central_polling;

	/*
	 * If devfreq_dev_release() was called by device_unregister() of
	 * _remove_devfreq(), we cannot mutex_lock(&devfreq->lock) and
	 * being_removed is already set. This also partially checks the case
	 * where devfreq_dev_release() is called from a thread other than
	 * the one called _remove_devfreq(); however, this case is
	 * dealt completely with another following being_removed check.
	 *
	 * Because being_removed is never being
	 * unset, we do not need to worry about race conditions on
	 * being_removed.
	 */
	if (devfreq->being_removed)
		return;

	if (central_polling)
		mutex_lock(&devfreq_list_lock);

	mutex_lock(&devfreq->lock);

	/*
	 * Check being_removed flag again for the case where
	 * devfreq_dev_release() was called in a thread other than the one
	 * possibly called _remove_devfreq().
	 */
	if (devfreq->being_removed) {
		mutex_unlock(&devfreq->lock);
		goto out;
	}

	/* devfreq->lock is unlocked and removed in _removed_devfreq() */
	_remove_devfreq(devfreq, true);

out:
	if (central_polling)
		mutex_unlock(&devfreq_list_lock);
}

/**
 * devfreq_monitor() - Periodically poll devfreq objects.
 * @work: the work struct used to run devfreq_monitor periodically.
 *
 */
static void devfreq_monitor(struct work_struct *work)
{
	static unsigned long last_polled_at;
	struct devfreq *devfreq, *tmp;
	int error;
	unsigned long jiffies_passed;
	unsigned long next_jiffies = ULONG_MAX, now = jiffies;
	struct device *dev;

	/* Initially last_polled_at = 0, polling every device at bootup */
	jiffies_passed = now - last_polled_at;
	last_polled_at = now;
	if (jiffies_passed == 0)
		jiffies_passed = 1;

	mutex_lock(&devfreq_list_lock);
	list_for_each_entry_safe(devfreq, tmp, &devfreq_list, node) {
		mutex_lock(&devfreq->lock);
		dev = devfreq->dev.parent;

		/* Do not remove tmp for a while */
		wait_remove_device = tmp;

		if (devfreq->governor->no_central_polling ||
		    devfreq->next_polling == 0) {
			mutex_unlock(&devfreq->lock);
			continue;
		}
		mutex_unlock(&devfreq_list_lock);

		/*
		 * Reduce more next_polling if devfreq_wq took an extra
		 * delay. (i.e., CPU has been idled.)
		 */
		if (devfreq->next_polling <= jiffies_passed) {
			error = update_devfreq(devfreq);

			/* Remove a devfreq with an error. */
			if (error && error != -EAGAIN) {

				dev_err(dev, "Due to update_devfreq error(%d), devfreq(%s) is removed from the device\n",
					error, devfreq->governor->name);

				/*
				 * Unlock devfreq before locking the list
				 * in order to avoid deadlock with
				 * find_device_devfreq or others
				 */
				mutex_unlock(&devfreq->lock);
				mutex_lock(&devfreq_list_lock);
				/* Check if devfreq is already removed */
				if (IS_ERR(find_device_devfreq(dev)))
					continue;
				mutex_lock(&devfreq->lock);
				/* This unlocks devfreq->lock and free it */
				_remove_devfreq(devfreq, false);
				continue;
			}
			devfreq->next_polling = devfreq->polling_jiffies;
		} else {
			devfreq->next_polling -= jiffies_passed;
		}

		if (devfreq->next_polling)
			next_jiffies = (next_jiffies > devfreq->next_polling) ?
					devfreq->next_polling : next_jiffies;

		mutex_unlock(&devfreq->lock);
		mutex_lock(&devfreq_list_lock);
	}
	wait_remove_device = NULL;
	mutex_unlock(&devfreq_list_lock);

	if (next_jiffies > 0 && next_jiffies < ULONG_MAX) {
		polling = true;
		queue_delayed_work(devfreq_wq, &devfreq_work, next_jiffies);
	} else {
		polling = false;
	}
}

/**
 * devfreq_add_device() - Add devfreq feature to the device
 * @dev:	the device to add devfreq feature.
 * @profile:	device-specific profile to run devfreq.
 * @governor:	the policy to choose frequency.
 * @data:	private data for the governor. The devfreq framework does not
 *		touch this value.
 */
struct devfreq *devfreq_add_device(struct device *dev,
				   struct devfreq_dev_profile *profile,
				   const struct devfreq_governor *governor,
				   void *data)
{
	struct devfreq *devfreq;
	int err = 0;

	if (!dev || !profile || !governor) {
		dev_err(dev, "%s: Invalid parameters.\n", __func__);
		return ERR_PTR(-EINVAL);
	}


	if (!governor->no_central_polling) {
		mutex_lock(&devfreq_list_lock);
		devfreq = find_device_devfreq(dev);
		mutex_unlock(&devfreq_list_lock);
		if (!IS_ERR(devfreq)) {
			dev_err(dev, "%s: Unable to create devfreq for the device. It already has one.\n", __func__);
			err = -EINVAL;
			goto out;
		}
	}

	devfreq = kzalloc(sizeof(struct devfreq), GFP_KERNEL);
	if (!devfreq) {
		dev_err(dev, "%s: Unable to create devfreq for the device\n",
			__func__);
		err = -ENOMEM;
		goto out;
	}

	mutex_init(&devfreq->lock);
	mutex_lock(&devfreq->lock);
	devfreq->dev.parent = dev;
	devfreq->dev.class = devfreq_class;
	devfreq->dev.release = devfreq_dev_release;
	devfreq->profile = profile;
	devfreq->governor = governor;
	devfreq->previous_freq = profile->initial_freq;
	devfreq->data = data;
	devfreq->next_polling = devfreq->polling_jiffies
			      = msecs_to_jiffies(devfreq->profile->polling_ms);
	devfreq->nb.notifier_call = devfreq_notifier_call;
	devfreq->qos_nb.notifier_call = devfreq_qos_notifier_call;

	/* Check the sanity of qos_list/qos_type */
	if (profile->qos_type || profile->qos_list) {
		int i;
		bool positive_corelation = false;

		if (profile->qos_type == PM_QOS_CPU_DMA_LATENCY ||
		    profile->qos_type == PM_QOS_NETWORK_LATENCY) {
			if (profile->qos_use_max) {
				dev_err(dev, "qos_use_max value inconsistent\n");
				err = -EINVAL;
			}
		} else {
			if (!profile->qos_use_max) {
				dev_err(dev, "qos_use_max value inconsistent\n");
				err = -EINVAL;
			}
		}
		if (err)
			goto err_dev;

		if (!profile->qos_type || !profile->qos_list) {
			dev_err(dev, "QoS requirement partially omitted.\n");
			err = -EINVAL;
			goto err_dev;
		}

		if (!profile->qos_list[0].freq) {
			dev_err(dev, "The first QoS requirement is the end of list.\n");
			err = -EINVAL;
			goto err_dev;
		}

		for (i = 1; profile->qos_list[i].freq; i++) {
			if (profile->qos_list[i].freq <=
			    profile->qos_list[i - 1].freq) {
				dev_err(dev, "qos_list[].freq not sorted in the ascending order. ([%d]=%lu, [%d]=%lu)\n",
					i - 1, profile->qos_list[i - 1].freq,
					i, profile->qos_list[i].freq);
				err = -EINVAL;
				goto err_dev;
			}

			if (i == 1) {
				if (profile->qos_list[0].qos_value <
				    profile->qos_list[1].qos_value)
					positive_corelation = true;
				continue;
			}

			if (((profile->qos_list[i - 1].qos_value <=
			      profile->qos_list[i].qos_value) &&
			     !positive_corelation)
			    ||
			    ((profile->qos_list[i - 1].qos_value >=
			      profile->qos_list[i].qos_value) &&
			     positive_corelation)) {
				dev_err(dev, "qos_list[].qos_value not sorted.\n");
				err = -EINVAL;
				goto err_dev;
			}
		}

		pm_qos_add_notifier(profile->qos_type, &devfreq->qos_nb);
	}

	dev_set_name(&devfreq->dev, dev_name(dev));
	err = device_register(&devfreq->dev);
	if (err) {
		put_device(&devfreq->dev);
		goto err_qos_add;
	}

	if (governor->init)
		err = governor->init(devfreq);
	if (err)
		goto err_init;

	mutex_unlock(&devfreq->lock);

	if (governor->no_central_polling)
		goto out;

	mutex_lock(&devfreq_list_lock);

	list_add(&devfreq->node, &devfreq_list);

	if (devfreq_wq && devfreq->next_polling && !polling) {
		polling = true;
		queue_delayed_work(devfreq_wq, &devfreq_work,
				   devfreq->next_polling);
	}
	mutex_unlock(&devfreq_list_lock);
	goto out;
err_init:
	device_unregister(&devfreq->dev);
err_qos_add:
	if (profile->qos_type || profile->qos_list)
		pm_qos_remove_notifier(profile->qos_type, &devfreq->qos_nb);
err_dev:
	mutex_unlock(&devfreq->lock);
	kfree(devfreq);
out:
	if (err)
		return ERR_PTR(err);
	else
		return devfreq;
}

/**
 * devfreq_remove_device() - Remove devfreq feature from a device.
 * @devfreq	the devfreq instance to be removed
 */
int devfreq_remove_device(struct devfreq *devfreq)
{
	bool central_polling;

	if (!devfreq)
		return -EINVAL;

	central_polling = !devfreq->governor->no_central_polling;

	if (central_polling) {
		mutex_lock(&devfreq_list_lock);
		while (wait_remove_device == devfreq) {
			mutex_unlock(&devfreq_list_lock);
			schedule();
			mutex_lock(&devfreq_list_lock);
		}
	}

	mutex_lock(&devfreq->lock);

	_remove_devfreq(devfreq, false); /* it unlocks devfreq->lock */

	if (central_polling)
		mutex_unlock(&devfreq_list_lock);

	return 0;
}

static ssize_t show_governor(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_devfreq(dev)->governor->name);
}

static ssize_t show_freq(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", to_devfreq(dev)->previous_freq);
}

static ssize_t show_polling_interval(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", to_devfreq(dev)->profile->polling_ms);
}

static ssize_t store_polling_interval(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct devfreq *df = to_devfreq(dev);
	unsigned int value;
	int ret;

	ret = sscanf(buf, "%u", &value);
	if (ret != 1)
		goto out;

	mutex_lock(&df->lock);
	df->profile->polling_ms = value;
	df->next_polling = df->polling_jiffies
			 = msecs_to_jiffies(value);
	mutex_unlock(&df->lock);

	ret = count;

	if (df->governor->no_central_polling)
		goto out;

	mutex_lock(&devfreq_list_lock);
	if (df->next_polling > 0 && !polling) {
		polling = true;
		queue_delayed_work(devfreq_wq, &devfreq_work,
				   df->next_polling);
	}
	mutex_unlock(&devfreq_list_lock);
out:
	return ret;
}

static ssize_t show_central_polling(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n",
		       !to_devfreq(dev)->governor->no_central_polling);
}

static ssize_t show_qos_min_freq(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	return sprintf(buf, "%lu\n", to_devfreq(dev)->qos_min_freq);
}

static ssize_t store_min_freq(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct devfreq *df = to_devfreq(dev);
	unsigned long value;
	int ret;
	unsigned long max;

	ret = sscanf(buf, "%lu", &value);
	if (ret != 1)
		goto out;

	mutex_lock(&df->lock);
	max = df->max_freq;
	if (value && max && value > max) {
		ret = -EINVAL;
		goto unlock;
	}

	df->min_freq = value;
	update_devfreq(df);
	ret = count;
unlock:
	mutex_unlock(&df->lock);
out:
	return ret;
}

static ssize_t show_min_freq(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%lu\n", to_devfreq(dev)->min_freq);
}

static ssize_t store_max_freq(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct devfreq *df = to_devfreq(dev);
	unsigned long value;
	int ret;
	unsigned long min;

	ret = sscanf(buf, "%lu", &value);
	if (ret != 1)
		goto out;

	mutex_lock(&df->lock);
	min = df->min_freq;
	if (value && min && value < min) {
		ret = -EINVAL;
		goto unlock;
	}

	df->max_freq = value;
	update_devfreq(df);
	ret = count;
unlock:
	mutex_unlock(&df->lock);
out:
	return ret;
}

static ssize_t show_max_freq(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%lu\n", to_devfreq(dev)->max_freq);
}

static struct device_attribute devfreq_attrs[] = {
	__ATTR(governor, S_IRUGO, show_governor, NULL),
	__ATTR(cur_freq, S_IRUGO, show_freq, NULL),
	__ATTR(central_polling, S_IRUGO, show_central_polling, NULL),
	__ATTR(polling_interval, S_IRUGO | S_IWUSR, show_polling_interval,
	       store_polling_interval),
	__ATTR(min_freq, S_IRUGO | S_IWUSR, show_min_freq, store_min_freq),
	__ATTR(max_freq, S_IRUGO | S_IWUSR, show_max_freq, store_max_freq),
	__ATTR(qos_min_freq, S_IRUGO, show_qos_min_freq, NULL),
	{ },
};

/**
 * devfreq_start_polling() - Initialize data structure for devfreq framework and
 *			   start polling registered devfreq devices.
 */
static int __init devfreq_start_polling(void)
{
	mutex_lock(&devfreq_list_lock);
	polling = false;
	devfreq_wq = create_freezable_workqueue("devfreq_wq");
	INIT_DELAYED_WORK_DEFERRABLE(&devfreq_work, devfreq_monitor);
	mutex_unlock(&devfreq_list_lock);

	devfreq_monitor(&devfreq_work.work);
	return 0;
}
late_initcall(devfreq_start_polling);

static int __init devfreq_init(void)
{
	devfreq_class = class_create(THIS_MODULE, "devfreq");
	if (IS_ERR(devfreq_class)) {
		pr_err("%s: couldn't create class\n", __FILE__);
		return PTR_ERR(devfreq_class);
	}
	devfreq_class->dev_attrs = devfreq_attrs;
	return 0;
}
subsys_initcall(devfreq_init);

static void __exit devfreq_exit(void)
{
	class_destroy(devfreq_class);
}
module_exit(devfreq_exit);

/*
 * The followings are helper functions for devfreq user device drivers with
 * OPP framework.
 */

/**
 * devfreq_recommended_opp() - Helper function to get proper OPP for the
 *			     freq value given to target callback.
 * @dev		The devfreq user device. (parent of devfreq)
 * @freq	The frequency given to target function
 * @floor	false: find LUB first and use GLB if LUB not available.
 *		true:  find GLB first and use LUB if GLB not available.
 *
 * LUB: least upper bound (at least this freq or above, but the least)
 * GLB: greatest lower bound (at most this freq or below, but the most)
 *
 */
struct opp *devfreq_recommended_opp(struct device *dev, unsigned long *freq,
				    bool floor)
{
	struct opp *opp;

	if (floor) {
		opp = opp_find_freq_floor(dev, freq);

		if (opp == ERR_PTR(-ENODEV))
			opp = opp_find_freq_ceil(dev, freq);
	} else {
		opp = opp_find_freq_ceil(dev, freq);

		if (opp == ERR_PTR(-ENODEV))
			opp = opp_find_freq_floor(dev, freq);
	}

	return opp;
}

/**
 * devfreq_register_opp_notifier() - Helper function to get devfreq notified
 *				   for any changes in the OPP availability
 *				   changes
 * @dev		The devfreq user device. (parent of devfreq)
 * @devfreq	The devfreq object.
 */
int devfreq_register_opp_notifier(struct device *dev, struct devfreq *devfreq)
{
	struct srcu_notifier_head *nh = opp_get_notifier(dev);

	if (IS_ERR(nh))
		return PTR_ERR(nh);
	return srcu_notifier_chain_register(nh, &devfreq->nb);
}

/**
 * devfreq_unregister_opp_notifier() - Helper function to stop getting devfreq
 *				     notified for any changes in the OPP
 *				     availability changes anymore.
 * @dev		The devfreq user device. (parent of devfreq)
 * @devfreq	The devfreq object.
 *
 * At exit() callback of devfreq_dev_profile, this must be included if
 * devfreq_recommended_opp is used.
 */
int devfreq_unregister_opp_notifier(struct device *dev, struct devfreq *devfreq)
{
	struct srcu_notifier_head *nh = opp_get_notifier(dev);

	if (IS_ERR(nh))
		return PTR_ERR(nh);
	return srcu_notifier_chain_unregister(nh, &devfreq->nb);
}

/**
 * In progress (prototyping)
 */
int devfreq_simple_ondemand_flexrate_do(struct devfreq *devfreq,
					unsigned long interval,
					unsigned long number)
{
	return 0;
}

MODULE_AUTHOR("MyungJoo Ham <myungjoo.ham@samsung.com>");
MODULE_DESCRIPTION("devfreq class support");
MODULE_LICENSE("GPL");
