#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/seqlock.h>
#include <linux/rtc.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/compiler_attributes.h>

static struct {
	ktime_t last_time;
	unsigned long last_jiffies;
	spinlock_t lock;
} state = { 0 };

struct timer_list timer;

static void reset_timer(void)
{
	/* In case if the timer wouldn't be read for a very long time,
	 * update the state by ourselves, so we won't loose the time because of
	 * jiffies overflow. */
	mod_timer(&timer, state.last_jiffies - 1);
}

static void update_time(void)
{
	unsigned long flags = 0;
	spin_lock_irqsave(&state.lock, flags);

	unsigned long ljiffies = jiffies;
	unsigned long delta = ljiffies - state.last_jiffies;
	state.last_time = ktime_add_ns(state.last_time, jiffies_to_nsecs(delta));
	state.last_jiffies = ljiffies;

	spin_unlock_irqrestore(&state.lock, flags);
}

static void virt_rtc_periodic_update(struct timer_list *t __always_unused)
{
	update_time();
	reset_timer();
}

static int virt_rtc_read_time(struct device *dev __always_unused,
			      struct rtc_time *tm)
{
	update_time();
	reset_timer();

	/* TODO: There is no need to use locks here, right?
	 * ktime_t should be of the word's width, so it's read should be atomic
	 * on the most architectures anyway... */
	unsigned long flags = 0;
	spin_lock_irqsave(&state.lock, flags);

	*tm = rtc_ktime_to_tm(state.last_time);

	spin_unlock_irqrestore(&state.lock, flags);
	return rtc_valid_tm(tm);
}

static int virt_rtc_set_time(struct device *dev __always_unused,
			     struct rtc_time *tm)
{
	unsigned long flags = 0;
	spin_lock_irqsave(&state.lock, flags);

	state.last_time = rtc_tm_to_ktime(*tm);
	state.last_jiffies = jiffies;

	spin_unlock_irqrestore(&state.lock, flags);

	return 0;
}

static struct rtc_device *rtcvirt;
static struct rtc_class_ops virt_rtc_ops = {
	.read_time = virt_rtc_read_time,
	.set_time = virt_rtc_set_time,
};

static struct {
	struct device *dev;
	struct class *class;
	dev_t devt;
} fake_dev;

static long init_fake_device(void)
{
	long err = 0;

	/* Don't create a device node. */
	fake_dev.devt = MKDEV(MAJOR(0), 0);

	fake_dev.class = class_create(THIS_MODULE, "virtrtc");
	if (IS_ERR(fake_dev.class)) {
		pr_err("failed to create virtrtc class\n");
		err = PTR_ERR(fake_dev.class);
		goto err;
	}

	fake_dev.dev = device_create(fake_dev.class, NULL, fake_dev.devt, NULL,
				     "virtrtc");
	if (IS_ERR(fake_dev.dev)) {
		pr_err("failed to create virtrtc device\n");
		err = PTR_ERR(fake_dev.dev);
		goto err_destroy_class;
	}

	return err;

err_destroy_class:
	class_destroy(fake_dev.class);
err:
	return err;
}

static int err_to_rc(long err)
{
	int ret = (int)err;
	if (unlikely(ret < err)) {
		BUG();
	}
	return ret;
}

static void destroy_fake_device(void)
{
	device_destroy(fake_dev.class, fake_dev.devt);
	class_destroy(fake_dev.class);
}

static int virt_rtc_init(void)
{
	long err = 0;

	err = init_fake_device();
	if (err < 0) {
		goto err;
	}

	/* NOTE: I'm not sure that this is a correct way to free resources.
	 * As far as I understand, the devres framework should release all resources as soon, as
	 * the device in question isn't referenced anymore (refcount == 0).
	 * But during the execution of the rtc_register_device() reference counter goes up.
	 * It seems that the only way to remove the references is to call rtc_device_unregister, but it's static
	 * and intended to be called by devres framework only.
	 * So it seems that there in no reasonable way to free the resources without using devres groups.
	 */
	/* Devres group to release resources on the module's exit path. */
	if (!devres_open_group(fake_dev.dev, fake_dev.dev, GFP_KERNEL)) {
		pr_err("failed to open devres group\n");
		goto err_destroy_device;
	}

	rtcvirt = devm_rtc_allocate_device(fake_dev.dev);
	if (IS_ERR(rtcvirt)) {
		pr_err("failed to create rtc device\n");
		err = PTR_ERR(rtcvirt);
		goto err_release_devres_group;
	}

	rtcvirt->ops = &virt_rtc_ops;

	state.last_time = 0;
	state.last_jiffies = jiffies;
	spin_lock_init(&state.lock);

	timer_setup(&timer, virt_rtc_periodic_update, 0);
	reset_timer();

	err = rtc_register_device(rtcvirt);
	if (err < 0) {
		pr_err("failed to register rtc device\n");
		goto err_del_timer;
	}

	return err_to_rc(err);

err_del_timer:
	del_timer(&timer);
err_release_devres_group:
	devres_release_group(fake_dev.dev, fake_dev.dev);
err_destroy_device:
	destroy_fake_device();
err:
	return err_to_rc(err);
}

static void virt_rtc_exit(void)
{
	del_timer(&timer);
	devres_release_group(fake_dev.dev, fake_dev.dev);
	destroy_fake_device();
}

module_init(virt_rtc_init);
module_exit(virt_rtc_exit);

MODULE_LICENSE("GPL");
