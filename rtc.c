#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/rtc.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/compiler_attributes.h>

/* Timer functions run in the interrupt context. It's not the brightest idea to use a mutex there.
 * TODO: Replace mutex with spinlock or seqlock. */

static struct {
	ktime_t last_time;
	unsigned long last_jiffies;
	struct mutex mx;
} state = { 0 };

struct timer_list timer;

static ktime_t time_passed(void)
{
	unsigned long delta = jiffies - state.last_jiffies;
	return jiffies_to_nsecs(delta);
}

static void reset_timer(void)
{
	/* In case if the timer wouldn't be read for a very long time,
	 * update the state by ourselves, so we won't loose the time because of
	 * jiffies overflow. */
	mod_timer(&timer, state.last_jiffies - 1);
}

static void update_time(void)
{
	mutex_lock(&state.mx);
	state.last_time += time_passed();
	state.last_jiffies = jiffies;
	mutex_unlock(&state.mx);
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

	*tm = rtc_ktime_to_tm(state.last_time);
	return rtc_valid_tm(tm);
}

static int virt_rtc_set_time(struct device *dev __always_unused,
			     struct rtc_time *tm)
{
	mutex_lock(&state.mx);
	state.last_jiffies = jiffies;
	state.last_time = rtc_tm_to_ktime(*tm);
	mutex_unlock(&state.mx);

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
	mutex_init(&state.mx);

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
	mutex_destroy(&state.mx);
	destroy_fake_device();
}

module_init(virt_rtc_init);
module_exit(virt_rtc_exit);

MODULE_LICENSE("GPL");
