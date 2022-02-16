#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/i2c.h>
#include <linux/delay.h>

/* AP3216C操作码定义 */
typedef enum enAP3216C_OPCODE {
    AP3216C_SYSTEMCONFIG    = 0x00,        /* 配置寄存器 */
    AP3216C_INTSTATUS       = 0x01,  	   /* 中断状态寄存器 */
    AP3216C_INTCLEAR        = 0x02,	       /* 中断清除寄存器 */
    AP3216C_START_ALL       = 0x03,        /* 开启全模式ALS/PS/IR */
    AP3216C_INIT            = 0x04,        /* 复位/初始化 */
    AP3216C_IRDATALOW       = 0x0A,	       /* IR数据低字节 */
    AP3216C_IRDATAHIGH      = 0x0B,	       /* IR数据高字节 */
    AP3216C_ALSDATALOW      = 0x0C,	       /* ALS数据低字节 */
    AP3216C_ALSDATAHIGH     = 0x0D,	       /* ALS数据高字节 */
    AP3216C_PSDATALOW       = 0x0E,	       /* PS数据低字节 */
    AP3216C_PSDATAHIGH      = 0x0F,	       /* PS数据高字节 */
} AP3216C_OPCODE;

typedef struct ap3216c {
	int major;
	struct class *class;
	struct i2c_client *client;
} ap3216c_t;

static ap3216c_t *gp_ap3216c;

/* 根据芯片手册，AP3216C不支持多字节连续读写 */
static int ap3216c_i2c_read(struct i2c_client *i2c, unsigned char address,
            unsigned char *data)
{
    struct i2c_msg msg[2];
    unsigned char reg = address;

    msg[0].addr = i2c->addr;
    msg[0].flags = !I2C_M_RD;
    msg[0].buf = &reg;
    msg[0].len = 1;

    msg[1].addr = i2c->addr;
    msg[1].flags = I2C_M_RD;
    msg[1].buf = data;
    msg[1].len = 1;

    if (i2c_transfer(i2c->adapter, msg, 2) != 2) {
        return -1;
    }
    return 0;
}

static int ap3216c_i2c_write(struct i2c_client *i2c, unsigned char address,
            unsigned char data)
{
    struct i2c_msg msg;
    unsigned char buff[2] = {address, data};

    msg.addr = i2c->addr;
    msg.flags = !I2C_M_RD;
    msg.buf = buff;
    msg.len = 2;

    if (i2c_transfer(i2c->adapter, &msg, 1) != 1) {
        return -1;
    }
    return 0;
}

static int ap3216c_open(struct inode *inode, struct file *filp)
{
    unsigned char value;

    if (ap3216c_i2c_write(gp_ap3216c->client, AP3216C_SYSTEMCONFIG,
        AP3216C_INIT) != 0)
        goto err;
    mdelay(50);
    /* 开启全模式ALS/PS/IR */
    if (ap3216c_i2c_write(gp_ap3216c->client, AP3216C_SYSTEMCONFIG,
                                AP3216C_START_ALL) != 0)
        goto err;
    if (ap3216c_i2c_read(gp_ap3216c->client, AP3216C_SYSTEMCONFIG, &value))
        goto err;

    if (value == AP3216C_START_ALL) {
        printk("[SKY DEBUG] AP3216C open success !\n");
        return 0;
    } else {
        printk("[SKY DEBUG] AP3216C open fail ! (%d)\n", value);
        goto err;
    }

err:
    return -EPERM;
}

static ssize_t ap3216c_read(struct file *filp, char __user *buf,
		size_t count, loff_t *ppos)
{
    int i;
    unsigned short buffer[4] = {0};
    unsigned char temp[6] = {0};

    if (count != sizeof(buffer)) {
        return -EPERM;
    }

    for (i = 0; i < 6; i++) {
        if (ap3216c_i2c_read(gp_ap3216c->client, AP3216C_IRDATALOW + i,
            &temp[i]) != 0)
            return -EIO;
    }

    /* IR */
    if (temp[0] & 0x80) {
        buffer[0] = 0;
    } else {
        buffer[0] = (unsigned short)((temp[1] << 2) | (temp[0] & 0x03));
    }
    /* ALS */
    buffer[1] = (unsigned short)(temp[3] << 8) | temp[2];
    /* PS */
    if (temp[4] & 0x40) {
        buffer[2] = 0;
    } else {
        buffer[2] = (unsigned short)(((temp[5] & 0x3f) << 4) | (temp[4] & 0x0f));
    }
    buffer[3] = ((temp[4] & 0x80) >> 7);

    if (copy_to_user(buf, buffer, sizeof(buffer))) {
        return -EFAULT;
    }
	return count;
}

static const struct file_operations ap3216c_fops = {
    .owner = THIS_MODULE,
    .open = ap3216c_open,
    .read = ap3216c_read,
};

static int ap3216c_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret;
    struct device *dev;

    gp_ap3216c = (ap3216c_t *)kmalloc(sizeof(ap3216c_t), GFP_KERNEL);
	if (IS_ERR(gp_ap3216c)) {
		ret = PTR_ERR(gp_ap3216c);
		goto kmalloc_err;
	}

	gp_ap3216c->major = register_chrdev(0, "ap3216c_chrdev", &ap3216c_fops);
	if (gp_ap3216c->major < 0) {
		ret = gp_ap3216c->major;
		goto register_chrdev_err;
	}

	gp_ap3216c->class = class_create(THIS_MODULE, "ap3216c_class");
	if (IS_ERR(gp_ap3216c->class)) {
		ret = PTR_ERR(gp_ap3216c->class);
		goto class_create_err;
	}

    dev = device_create(gp_ap3216c->class, NULL, MKDEV(gp_ap3216c->major, 0),
        NULL, "ap3216c");
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        goto device_create_err;
    }

    gp_ap3216c->client = client;

    printk("[SKY DEBUG] %s line : %d\n", __FUNCTION__, __LINE__);
    return 0;

device_create_err:
	class_destroy(gp_ap3216c->class);
class_create_err:
	unregister_chrdev(gp_ap3216c->major, "ap3216c_chrdev");
register_chrdev_err:
	kfree(gp_ap3216c);
kmalloc_err:
	return ret;
}

static int ap3216c_i2c_remove(struct i2c_client *client)
{
    device_destroy(gp_ap3216c->class, MKDEV(gp_ap3216c->major, 0));
    class_destroy(gp_ap3216c->class);
	unregister_chrdev(gp_ap3216c->major, "ap3216c_chrdev");
	kfree(gp_ap3216c);
    printk("[SKY DEBUG] %s line : %d\n", __FUNCTION__, __LINE__);
    return 0;
}


/*定义ID 匹配表*/
static const struct i2c_device_id ap3216c_device_id[] = {
    {"ap3216c", 0},
    {/* sentinel */},
};
MODULE_DEVICE_TABLE(i2c, ap3216c_device_id);

/*定义设备树匹配表*/
static const struct of_device_id ap3216c_of_match[] = {
    {.compatible = "sky,ap3216c"},
    {/* sentinel */},
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

static struct i2c_driver ap3216c_i2c_driver = {
    .driver = {
        .name = "ap3216c_i2c",
        .of_match_table = ap3216c_of_match,
    },
    .id_table = ap3216c_device_id,
    .probe = ap3216c_i2c_probe,
    .remove = ap3216c_i2c_remove,
};

module_i2c_driver(ap3216c_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sky");
MODULE_DESCRIPTION("rk3399 Tinker_board_2S ap3216c_i2c driver");