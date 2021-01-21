#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include "address_map_arm.h"


/* Kernel character device driver. By default, this driver provides the text "Hello from
 * chardev" when /dev/chardev is read (for example, cat /dev/chardev). The text can be changed
 * by writing a new string to /dev/chardev (for example echo "New message" > /dev/chardev).
 * This version of the code uses copy_to_user and copy_from_user, to send data to, and receive
 * date from, user-space */

static int led_open (struct inode *, struct file *);
static int led_release (struct inode *, struct file *);
//static ssize_t led_read (struct file *, char *, size_t, loff_t *);
static ssize_t led_write(struct file *, const char *, size_t, loff_t *);

static int hex_open (struct inode *, struct file *);
static int hex_release (struct inode *, struct file *);
//static ssize_t hex_read (struct file *, char *, size_t, loff_t *);
static ssize_t hex_write(struct file *, const char *, size_t, loff_t *);

#define SUCCESS 0
#define DEVICE_NAME1 "LEDR"
#define DEVICE_NAME2 "HEX"

void *LW_virtual;
volatile unsigned *LEDR_ptr, *HEX3_HEX0_ptr, *HEX5_HEX4_ptr;


static dev_t LEDR_no = 0;
static dev_t HEX_no = 0;
static struct cdev *LEDR_cdev = NULL;
static struct cdev *HEX_cdev = NULL;
static struct class *LEDR_class = NULL;
static struct class *HEX_class = NULL;

#define MAX_SIZE_LEDR 5	// we assume that no message longer than this will be used
#define MAX_SIZE_HEX 8	// we assume that no message longer than this will be used
static char LEDR_msg[MAX_SIZE_LEDR];	// the character array that can be read or written
static char HEX_msg[MAX_SIZE_HEX];

int hex(int num){
	switch(num){
		case 0:
			return 0x3F;
		case 1:
			return 0x06;
		case 2:
			return 0x5B;
		case 3:
			return 0x4F;
		case 4:
			return 0x66;
		case 5:
			return 0x6D;
		case 6:
			return 0x7D;
		case 7:
			return 0x07;
		case 8:
			return 0x7F;
		case 9:
			return 0x67;
	}
}

static struct file_operations fops1 = {
	.owner = THIS_MODULE,
//	.read = key_read,
	.write = led_write,
	.open = led_open,
	.release = led_release
};

static struct file_operations fops2 = {
	.owner = THIS_MODULE,
//	.read = sw_read,
	.write = hex_write,
	.open = hex_open,
	.release = hex_release
};

static int __init init_drivers(void)
{
	int err = 0;

    LW_virtual = ioremap_nocache(LW_BRIDGE_BASE, LW_BRIDGE_SPAN);
    LEDR_ptr = LW_virtual + LEDR_BASE;
    HEX3_HEX0_ptr = LW_virtual + HEX3_HEX0_BASE;
    HEX5_HEX4_ptr = LW_virtual + HEX5_HEX4_BASE;
   // *(KEY_ptr + 0x3) = 0xF;

    *(LEDR_ptr) = 0x0;

	/* Get a device number. Get one minor number (0) */
	if ((err = alloc_chrdev_region (&LEDR_no, 0, 1, DEVICE_NAME1)) < 0) {
		printk (KERN_ERR "LEDR: alloc_chrdev_region() failed with return value %d\n", err);
		return err;
	}

	if ((err = alloc_chrdev_region (&HEX_no, 0, 1, DEVICE_NAME2)) < 0) {
		printk (KERN_ERR "HEX: alloc_chrdev_region() failed with return value %d\n", err);
		return err;
	}

	// Allocate and initialize the character device
	LEDR_cdev = cdev_alloc ();
	LEDR_cdev->ops = &fops1;
	LEDR_cdev->owner = THIS_MODULE;

    HEX_cdev = cdev_alloc ();
	HEX_cdev->ops = &fops2;
	HEX_cdev->owner = THIS_MODULE;

	// Add the character device to the kernel
	if ((err = cdev_add (LEDR_cdev, LEDR_no, 1)) < 0) {
		printk (KERN_ERR "LEDR: cdev_add() failed with return value %d\n", err);
		return err;
	}

	if ((err = cdev_add (HEX_cdev, HEX_no, 1)) < 0) {
		printk (KERN_ERR "HEX: cdev_add() failed with return value %d\n", err);
		return err;
	}


	LEDR_class = class_create (THIS_MODULE, DEVICE_NAME1);
	device_create (LEDR_class, NULL, LEDR_no, NULL, DEVICE_NAME1 );

	HEX_class = class_create (THIS_MODULE, DEVICE_NAME2);
	device_create (HEX_class, NULL, HEX_no, NULL, DEVICE_NAME2 );


	return 0;
}

static void __exit stop_drivers(void)
{

	*(LEDR_ptr) = 0x0;
    *(HEX3_HEX0_ptr) = 0x0;
    *(HEX5_HEX4_ptr) = 0x0;

	device_destroy (LEDR_class, LEDR_no);
	cdev_del (LEDR_cdev);
	class_destroy (LEDR_class);
	unregister_chrdev_region (LEDR_no, 1);

	device_destroy (HEX_class,HEX_no);
	cdev_del (HEX_cdev);
	class_destroy (HEX_class);
	unregister_chrdev_region (HEX_no, 1);
}

/* Called when a process opens led */
static int led_open(struct inode *inode, struct file *file)
{
	return SUCCESS;
}

/* Called when a process closes led */
static int led_release(struct inode *inode, struct file *file)
{
	return 0;
}


/* Called when a process writes to led */
static ssize_t led_write(struct file *filp, const char *buffer, size_t length, loff_t *offset)
{
	size_t bytes; int num;
	bytes = length;

	if (bytes > MAX_SIZE_LEDR - 1)	// can copy all at once, or not?
		bytes = MAX_SIZE_LEDR - 1;
	if (copy_from_user (LEDR_msg, buffer, bytes) != 0)
		printk (KERN_ERR "Error: copy_from_user unsuccessful");

	//LEDR_msg[bytes] = '\0';	// NULL terminate
	sscanf(LEDR_msg, "%x", &num);

	*LEDR_ptr = num;
	// Note: we do NOT update *offset; we just copy the data into chardev_msg
	return bytes;
}


/* Called when a process opens hex*/
static int hex_open(struct inode *inode, struct file *file)
{
	return SUCCESS;
}

/* Called when a process closes hex*/
static int hex_release(struct inode *inode, struct file *file)
{
	return 0;
}

/* Called when a process writes to hex */
static ssize_t hex_write(struct file *filp, const char *buffer, size_t length, loff_t *offset)
{
	size_t bytes;
	bytes = length;

    int disp0, disp1, disp2, disp3, disp4, disp5;
    int num;

	if (bytes > MAX_SIZE_HEX - 1)	// can copy all at once, or not?
		bytes = MAX_SIZE_HEX - 1;
	if (copy_from_user (HEX_msg, buffer, bytes) != 0)
		printk (KERN_ERR "Error: copy_from_user unsuccessful");

    sscanf(HEX_msg, "%d", &num);

    disp0 = num %10; 		        //first digit
    disp1 = (num %100)/10;		    //second digit
    disp2 = (num %1000)/100;	    //third digit
    disp3 = (num %10000)/1000;	    //fourth digit
    disp4 = (num %100000)/10000;	//fifth digit
    disp5 = (num %1000000)/100000;	//sixth digit


    *(HEX3_HEX0_ptr) = hex(disp0)| (hex(disp1)<<8) | (hex(disp2) << 16) | (hex(disp3) << 24);
    *(HEX5_HEX4_ptr) = hex(disp4) | (hex(disp5) <<8);

	return bytes;
}


MODULE_LICENSE("GPL");
module_init (init_drivers);
module_exit (stop_drivers);
