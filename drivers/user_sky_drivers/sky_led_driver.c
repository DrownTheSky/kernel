#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>

#define DEV_FIFO_TYPE 'k'
#define DEV_FIFO_CLEAN _IO(DEV_FIFO_TYPE, 0)
#define DEV_FIFO_GET _IOR(DEV_FIFO_TYPE, 1, int)
#define DEV_FIFO_SET _IOW(DEV_FIFO_TYPE, 2, int)

typedef struct leds {
	int major;
	struct class *class;
	struct gpio_descs *leds;
} leds_t;

static leds_t *pg_leds;

static int leds_open(struct inode *inode, struct file *filp)
{
	switch (iminor(inode)) {
	case 0:
		filp->private_data = pg_leds->leds->desc[0];
		break;
	case 1:
		filp->private_data = pg_leds->leds->desc[1];
		break;
	case 2:
		filp->private_data = pg_leds->leds->desc[2];
		break;
	default :
		return -1;
	}
	return 0;
}

static int leds_release(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;
	return 0;
}

static ssize_t leds_read(struct file *filp, char __user *buf,
		size_t count, loff_t *ppos)
{
	int status;
	struct gpio_desc *private;

	private = (struct gpio_desc *)(filp->private_data);
	status = gpiod_get_value(private);
    if (copy_to_user(buf, &status, sizeof(status))) {
		return -EFAULT;
	}
	// printk("status = %d\n", status);
	return count;
}

static ssize_t leds_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *ppos)
{
	int status;
	struct gpio_desc *private;

	private = (struct gpio_desc *)(filp->private_data);
	if (copy_from_user(&status, buf, sizeof(status))) {
		return -EFAULT;
	}
    gpiod_set_value(private, status);
	// printk("status = %d\n", status);
    return count;
}

static long leds_ioctl(struct file *filp, u_int cmd, u_long arg)
{
    long err = 0;
	int value = 0;
	struct gpio_desc *private;
    void __user *argp = (void __user *)arg;
	private = (struct gpio_desc *)(filp->private_data);

	if (_IOC_TYPE(cmd) != DEV_FIFO_TYPE) {
		pr_err("[cmd %x, magic %x != %x]\n",cmd, _IOC_TYPE(cmd), DEV_FIFO_TYPE);
		return -ENOTTY;
	}
    if (_IOC_NR(cmd) & _IOC_READ) {
        err = !access_ok(VERIFY_READ, argp, _IOC_SIZE(cmd));
    } else if (_IOC_NR(cmd) & _IOC_WRITE) {
        err = !access_ok(VERIFY_WRITE, argp, _IOC_SIZE(cmd));
    }
    if (err) {
        pr_err("bad access %ld\n", err);
        return -EFAULT;
    }

    switch(cmd) {
    case DEV_FIFO_CLEAN:
		gpiod_set_value(private, 0);
        break;
    case DEV_FIFO_GET:
		value = gpiod_get_value(private);
        err = put_user(value, (int __user *)argp);
        break;
    case DEV_FIFO_SET:
        err = get_user(value, (int __user *)argp);
		gpiod_set_value(private, value);
        break;
    default:
        return -EINVAL;
    }
    return err;
}

static const struct file_operations leds_fileops = {
	.owner = THIS_MODULE,
	.open = leds_open,
	.release = leds_release,
	.read = leds_read,
	.write = leds_write,
	.unlocked_ioctl = leds_ioctl,
};

static int leds_driver_probe(struct platform_device *pdev)
{
    int ret;
	int i;
	struct device *dev;

	pg_leds = (leds_t *)kmalloc(sizeof(leds_t), GFP_KERNEL);
	if (IS_ERR(pg_leds)) {
		ret = PTR_ERR(pg_leds);
		goto kmalloc_err;
	}

	pg_leds->major = register_chrdev(0, "leds_chrdev", &leds_fileops);
	if (pg_leds->major < 0) {
		ret = pg_leds->major;
		goto register_chrdev_err;
	}

	pg_leds->class = class_create(THIS_MODULE, "leds_class");
	if (IS_ERR(pg_leds->class)) {
		ret = PTR_ERR(pg_leds->class);
		goto class_create_err;
	}

	for (i = 0; i < 3; i++) {
		dev = device_create(pg_leds->class, NULL, MKDEV(pg_leds->major, i),
			NULL, "led%d", i);
		if (IS_ERR(dev)) {
			ret = PTR_ERR(dev);
			goto device_create_err;
		}
	}

	pg_leds->leds = gpiod_get_array(&pdev->dev, NULL, GPIOD_OUT_HIGH);
	if (IS_ERR(pg_leds->leds)) {
		ret = PTR_ERR(pg_leds->leds);
		goto gpiod_get_array_err;
	}

    for (i = 0; i < 3; i++) {
        gpiod_direction_output(pg_leds->leds->desc[i], 0);
    }

    printk("[SKY DEBUG] %s line : %d\n", __FUNCTION__, __LINE__);
	return 0;

gpiod_get_array_err:
	for(i = 0; i < 3; i++) {
		device_destroy(pg_leds->class, MKDEV(pg_leds->major, i));
	}
device_create_err:
	class_destroy(pg_leds->class);
class_create_err:
	unregister_chrdev(pg_leds->major, "leds_chrdev");
register_chrdev_err:
	kfree(pg_leds);
kmalloc_err:
	return ret;
}

static int leds_driver_remove(struct platform_device *pdev)
{
	int i;

	gpiod_put_array(pg_leds->leds);
	for(i = 0; i < 3; i++) {
		device_destroy(pg_leds->class, MKDEV(pg_leds->major, i));
	}
	class_destroy(pg_leds->class);
	unregister_chrdev(pg_leds->major, "leds_chrdev");
	kfree(pg_leds);
    printk("[SKY DEBUG] %s line : %d\n", __FUNCTION__, __LINE__);
	return 0;
}

static const struct of_device_id leds_of_match[] = {
	{.compatible = "sky,leds"},
	{},
};
MODULE_DEVICE_TABLE(of, leds_of_match);

static struct platform_driver leds_driver = {
    .probe = leds_driver_probe,
    .remove = leds_driver_remove,
	.driver = {
		.name = "leds_driver",
		.of_match_table = leds_of_match,
	},
};

module_platform_driver(leds_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sky");
MODULE_DESCRIPTION("rk3399 Tinker_board_2S led driver");