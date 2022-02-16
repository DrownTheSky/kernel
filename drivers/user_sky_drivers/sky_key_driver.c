#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/timer.h>
#include <linux/poll.h>

typedef struct keys {
	int major;
	struct class *class;
	struct gpio_descs *keys;
	int irq[2];
	struct timer_list timer[2];
	int key_flag[2];
} keys_t;

static keys_t *pg_keys;
static DECLARE_WAIT_QUEUE_HEAD(keys_wait);
static struct fasync_struct *gt_fasync;

static int keys_open(struct inode *inode, struct file *filp)
{
	pg_keys->key_flag[iminor(inode)] = 0;
	return 0;
}

static ssize_t keys_read(struct file *filp, char __user *buf,
		size_t count, loff_t *ppos)
{
    int minor = iminor(file_inode(filp));

    if ((pg_keys->key_flag[minor] == 0) &&
		(filp->f_flags & O_NONBLOCK)) {		// 非阻塞
        return -EAGAIN;
    }

    wait_event_interruptible(keys_wait, pg_keys->key_flag[minor]);

    if (copy_to_user(buf, &pg_keys->key_flag[minor], sizeof(int))) {
		return -EFAULT;
	}
	pg_keys->key_flag[minor] = 0;
	return count;
}

static unsigned int keys_poll(struct file *filp, poll_table *wait)
{
	int minor = iminor(file_inode(filp));
	poll_wait(filp, &keys_wait, wait);
    return pg_keys->key_flag[minor] ? (POLLIN | POLLRDNORM) : 0;
}

static int keys_fasync(int fd, struct file *filp, int on)
{
	return fasync_helper(fd, filp, on, &gt_fasync);
}

static const struct file_operations keys_fileops = {
	.owner = THIS_MODULE,
	.open = keys_open,
	.read = keys_read,
    .poll = keys_poll,
    .fasync = keys_fasync,
};

static irqreturn_t gpio_key_isr_fn(int irq, void *dev)
{
	struct timer_list *timer = (struct timer_list *)dev;
    /* 重新设置超时时间 */
    mod_timer(timer, jiffies + msecs_to_jiffies(10));
	return IRQ_HANDLED;
}

static void gpio_key_timer_fn(unsigned long data)
{
	int status;
	int minor = data;

	status = gpiod_get_value(pg_keys->keys->desc[minor]);
	if (status) {
		pg_keys->key_flag[minor] = 1;
		wake_up_interruptible(&keys_wait);
		kill_fasync(&gt_fasync, SIGIO, POLL_IN);
	}
}

static int keys_driver_probe(struct platform_device *pdev)
{
    int ret;
	int i;
	struct device *dev;

	pg_keys = (keys_t *)kmalloc(sizeof(keys_t), GFP_KERNEL);
	if (IS_ERR(pg_keys)) {
		ret = PTR_ERR(pg_keys);
		goto kmalloc_err;
	}

	pg_keys->major = register_chrdev(0, "keys_chrdev", &keys_fileops);
	if (pg_keys->major < 0) {
		ret = pg_keys->major;
		goto register_chrdev_err;
	}

	pg_keys->class = class_create(THIS_MODULE, "keys_class");
	if (IS_ERR(pg_keys->class)) {
		ret = PTR_ERR(pg_keys->class);
		goto class_create_err;
	}

	for (i = 0; i < 2; i++) {
		dev = device_create(pg_keys->class, NULL, MKDEV(pg_keys->major, i),
			NULL, "key%d", i);
		if (IS_ERR(dev)) {
			ret = PTR_ERR(dev);
			goto device_create_err;
		}
	}

	pg_keys->keys = gpiod_get_array(&pdev->dev, NULL, GPIOD_IN);
	if (IS_ERR(pg_keys->keys)) {
		ret = PTR_ERR(pg_keys->keys);
		goto gpiod_get_array_err;
	}

	for (i = 0; i < 2; i++) {
        gpiod_direction_input(pg_keys->keys->desc[i]);
		pg_keys->irq[i] = gpiod_to_irq(pg_keys->keys->desc[i]);
        ret = request_irq(pg_keys->irq[i], gpio_key_isr_fn,
			IRQF_TRIGGER_RISING,
			"key_irq", &pg_keys->timer[i]);
		if (ret < 0) {
			pr_err("can't get IRQ %d\n", pg_keys->irq[i]);
			goto request_irq_err;
		}

        setup_timer(&pg_keys->timer[i], gpio_key_timer_fn, i);
        pg_keys->timer[i].expires = jiffies;
		add_timer(&pg_keys->timer[i]);

		pg_keys->key_flag[i] = 0;
	}

    printk("[SKY DEBUG] %s line : %d\n", __FUNCTION__, __LINE__);
	return 0;


request_irq_err:
	gpiod_put_array(pg_keys->keys);
gpiod_get_array_err:
	for(i = 0; i < 2; i++) {
		device_destroy(pg_keys->class, MKDEV(pg_keys->major, i));
	}
device_create_err:
	class_destroy(pg_keys->class);
class_create_err:
	unregister_chrdev(pg_keys->major, "keys_chrdev");
register_chrdev_err:
	kfree(pg_keys);
kmalloc_err:
	return ret;
}

static int keys_driver_remove(struct platform_device *pdev)
{
	int i;
    for (i = 0; i < 2; i++) {
		del_timer_sync(&pg_keys->timer[i]);
        free_irq(pg_keys->irq[i], &pg_keys->timer[i]);
    }
	gpiod_put_array(pg_keys->keys);
	for(i = 0; i < 2; i++) {
		device_destroy(pg_keys->class, MKDEV(pg_keys->major, i));
	}
	class_destroy(pg_keys->class);
	unregister_chrdev(pg_keys->major, "keys_chrdev");
	kfree(pg_keys);
    printk("[SKY DEBUG] %s line : %d\n", __FUNCTION__, __LINE__);
	return 0;
}

static const struct of_device_id keys_of_match[] = {
	{.compatible = "sky,keys"},
	{},
};
MODULE_DEVICE_TABLE(of, keys_of_match);

static struct platform_driver keys_driver = {
    .probe = keys_driver_probe,
    .remove = keys_driver_remove,
	.driver = {
		.name = "keys_driver",
		.of_match_table = keys_of_match,
	},
};

module_platform_driver(keys_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sky");
MODULE_DESCRIPTION("rk3399 Tinker_board_2S key driver");
