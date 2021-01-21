#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <asm/io.h>
#include <asm/uaccess.h>
//#include "address_map_arm.h"

/* Kernel character device driver. By default, this driver provides the text "Hello from
 * chardev" when /dev/chardev is read (for example, cat /dev/chardev). The text can be changed
 * by writing a new string to /dev/chardev (for example echo "New message" > /dev/chardev).
 * This version of the code uses copy_to_user and copy_from_user, to send data to, and receive
 * date from, user-space */

static int key_open (struct inode *, struct file *);
static int key_release (struct inode *, struct file *);
static ssize_t key_read (struct file *, char *, size_t, loff_t *);
//static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

static int sw_open (struct inode *, struct file *);
static int sw_release (struct inode *, struct file *);
static ssize_t sw_read (struct file *, char *, size_t, loff_t *);

#define SUCCESS 0
#define DEVICE_NAME1 "KEY"
#define DEVICE_NAME2 "SW"

void *LW_virtual;
volatile unsigned *KEY_ptr, *SW_ptr;


static dev_t KEY_no = 0;
static dev_t SW_no = 0;
static struct cdev *KEY_cdev = NULL;
static struct cdev *SW_cdev = NULL;
static struct class *KEY_class = NULL;
static struct class *SW_class = NULL;

#define MAX_SIZE_KEY 2	// we assume that no message longer than this will be used
#define MAX_SIZE_SW 4	// we assume that no message longer than this will be used
//static char chardev_msg[MAX_SIZE];	// the character array that can be read or written

static struct file_operations fops1 = {
	.owner = THIS_MODULE,
	.read = key_read,
//	.write = key_write,
	.open = key_open,
	.release = key_release
};

static struct file_operations fops2 = {
	.owner = THIS_MODULE,
	.read = sw_read,
//	.write = sw_write,
	.open = sw_open,
	.release = sw_release
};

static int __init init_drivers(void)
{
	int err = 0;

    LW_virtual = ioremap_nocache(0xFF200000, 0x5000);
    SW_ptr = LW_virtual + 0x40;
    KEY_ptr = LW_virtual + 0x50;
    *(KEY_ptr + 0x3) = 0xF;

	/* Get a device number. Get one minor number (0) */
	if ((err = alloc_chrdev_region (&KEY_no, 0, 1, DEVICE_NAME1)) < 0) {
		printk (KERN_ERR "KEY: alloc_chrdev_region() failed with return value %d\n", err);
		return err;
	}

	if ((err = alloc_chrdev_region (&SW_no, 0, 1, DEVICE_NAME2)) < 0) {
		printk (KERN_ERR "SW: alloc_chrdev_region() failed with return value %d\n", err);
		return err;
	}

	// Allocate and initialize the character device
	KEY_cdev = cdev_alloc ();
	KEY_cdev->ops = &fops1;
	KEY_cdev->owner = THIS_MODULE;

    SW_cdev = cdev_alloc ();
	SW_cdev->ops = &fops2;
	SW_cdev->owner = THIS_MODULE;

	// Add the character device to the kernel
	if ((err = cdev_add (KEY_cdev, KEY_no, 1)) < 0) {
		printk (KERN_ERR "KEY: cdev_add() failed with return value %d\n", err);
		return err;
	}

	if ((err = cdev_add (SW_cdev, SW_no, 1)) < 0) {
		printk (KERN_ERR "SW: cdev_add() failed with return value %d\n", err);
		return err;
	}


	KEY_class = class_create (THIS_MODULE, DEVICE_NAME1);
	device_create (KEY_class, NULL, KEY_no, NULL, DEVICE_NAME1 );

	SW_class = class_create (THIS_MODULE, DEVICE_NAME2);
	device_create (SW_class, NULL, SW_no, NULL, DEVICE_NAME2 );


	return 0;
}

static void __exit stop_drivers(void)
{

	*(KEY_ptr + 0x3) = 0xF;

	device_destroy (KEY_class, KEY_no);
	cdev_del (KEY_cdev);
	class_destroy (KEY_class);
	unregister_chrdev_region (KEY_no, 1);

	device_destroy (SW_class,SW_no);
	cdev_del (SW_cdev);
	class_destroy (SW_class);
	unregister_chrdev_region (SW_no, 1);
}

/* Called when a process opens chardev */
static int key_open(struct inode *inode, struct file *file)
{
	return SUCCESS;
}

/* Called when a process closes chardev */
static int key_release(struct inode *inode, struct file *file)
{
	return 0;
}

/* Called when a process reads from chardev. Provides character data from chardev_msg.
 * Returns, and sets *offset to, the number of bytes read. */
static ssize_t key_read(struct file *filp, char *buffer, size_t length, loff_t *offset)
{
    char themessage[MAX_SIZE_KEY + 1];
	size_t bytes;

	bytes = MAX_SIZE_KEY - (*offset);

	if (bytes){
        sprintf(themessage, "%03X\n", *(KEY_ptr + 0x3));

        copy_to_user(buffer, themessage, bytes);

	}

	(*offset) = bytes;

    *(KEY_ptr + 0x3) = 0xF;
	return bytes;

}

static int sw_open(struct inode *inode, struct file *file)
{
	return SUCCESS;
}

/* Called when a process closes chardev */
static int sw_release(struct inode *inode, struct file *file)
{
	return 0;
}

/* Called when a process reads from chardev. Provides character data from chardev_msg.
 * Returns, and sets *offset to, the number of bytes read. */
static ssize_t sw_read(struct file *filp, char *buffer, size_t length, loff_t *offset)
{
    char themessage[MAX_SIZE_SW + 1] = "";
	size_t bytes;

	bytes = MAX_SIZE_SW - (*offset);

	if (bytes){
        sprintf(themessage, "%X\n", *(SW_ptr));

        copy_to_user(buffer, themessage, bytes);

	}

	(*offset) = bytes;

	return bytes;
}



MODULE_LICENSE("GPL");
module_init (init_drivers);
module_exit (stop_drivers);
