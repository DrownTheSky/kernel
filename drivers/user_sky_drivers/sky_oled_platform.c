#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include "sky_oled_common.h"

typedef struct show {
	uint8_t flag;
	uint8_t x;
	uint8_t y;
	char str[20];
	uint8_t size;
} show_t;

oled_t *gp_oled;
struct oled_operation *gp_oled_operations;

static ssize_t oled_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *ppos)
{
	show_t show;
	if (copy_from_user(&show, buf, sizeof(show))) {
		return -EFAULT;
	}
	switch (show.flag)
	{
	case 0:
	case 1:
		gp_oled_operations->display(show.flag);
		break;
	case 2:
		gp_oled_operations->clear();
		break;
	case 3:
		gp_oled_operations->str(show.x, show.y, show.size, show.str);
		break;
	default:
		return -EPERM;
	}
    return count;
}

static const struct file_operations oled_fileops = {
	.owner = THIS_MODULE,
	.write = oled_write,
};

static int oled_driver_probe(struct platform_device *pdev)
{
    int ret;
	int i;
	struct device *dev;

	gp_oled = (oled_t *)kmalloc(sizeof(oled_t), GFP_KERNEL);
	if (IS_ERR(gp_oled)) {
		ret = PTR_ERR(gp_oled);
		goto kmalloc_err;
	}

	gp_oled->major = register_chrdev(0, "oled_chrdev", &oled_fileops);
	if (gp_oled->major < 0) {
		ret = gp_oled->major;
		goto register_chrdev_err;
	}

	gp_oled->class = class_create(THIS_MODULE, "oled_class");
	if (IS_ERR(gp_oled->class)) {
		ret = PTR_ERR(gp_oled->class);
		goto class_create_err;
	}

	dev = device_create(gp_oled->class, NULL, MKDEV(gp_oled->major, 0),
		NULL, "oled");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto device_create_err;
	}

	gp_oled->gpiod = gpiod_get_array(&pdev->dev, NULL, GPIOD_OUT_LOW);
	if (IS_ERR(gp_oled->gpiod)) {
		ret = PTR_ERR(gp_oled->gpiod);
		goto gpiod_get_array_err;
	}

    for (i = 0; i < 4; i++) {
        gpiod_direction_output(gp_oled->gpiod->desc[i], 0);
    }

	gp_oled_operations = get_oled_operation();
	gp_oled_operations->init();

    printk("[SKY DEBUG] %s line : %d\n", __FUNCTION__, __LINE__);
	return 0;

gpiod_get_array_err:
	device_destroy(gp_oled->class, MKDEV(gp_oled->major, 0));
device_create_err:
	class_destroy(gp_oled->class);
class_create_err:
	unregister_chrdev(gp_oled->major, "oled_chrdev");
register_chrdev_err:
	kfree(gp_oled);
kmalloc_err:
	return ret;
}

static int oled_driver_remove(struct platform_device *pdev)
{
	gpiod_put_array(gp_oled->gpiod);
	device_destroy(gp_oled->class, MKDEV(gp_oled->major, 0));
	class_destroy(gp_oled->class);
	unregister_chrdev(gp_oled->major, "oled_chrdev");
	kfree(gp_oled);
    printk("[SKY DEBUG] %s line : %d\n", __FUNCTION__, __LINE__);
	return 0;
}

static const struct of_device_id oled_of_match[] = {
	{.compatible = "sky,oled"},
	{},
};
MODULE_DEVICE_TABLE(of, oled_of_match);

static struct platform_driver oled_driver = {
    .probe = oled_driver_probe,
    .remove = oled_driver_remove,
	.driver = {
		.name = "oled_driver",
		.of_match_table = oled_of_match,
	},
};

module_platform_driver(oled_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sky");
MODULE_DESCRIPTION("rk3399 Tinker_board_2S oled driver");