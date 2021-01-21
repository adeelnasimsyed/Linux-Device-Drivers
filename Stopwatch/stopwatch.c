#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/interrupt.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include "address_map_arm.h"
#include "interrupt_ID.h"


//The functions for the character device driver
static int stopwatch_open (struct inode *, struct file *);
static int stopwatch_release (struct inode *, struct file *);
static ssize_t stopwatch_read (struct file *, char *, size_t, loff_t *);
static ssize_t stopwatch_write (struct file *, const char *, size_t, loff_t *);

//Global variables to track time, stopwatch initializes at 59:59:99
int min = 59; int sec = 59; int ms = 99;
int time;

//Is set when "disp" is written to device driver
bool disp = false;

//Pointer for LW Bridge, timer and hex
void * LW_virtual;
volatile int *timer_ptr, *HEX_3_0_ptr, *HEX_4_5_ptr;

//define name of character device driver
#define SUCCESS 0
#define DEVICE_NAME "stopwatch"

//Pointers for character device driver
static dev_t stopwatch_no = 0;
static struct cdev *stopwatch_cdev = NULL;
static struct class *stopwatch_class = NULL;

// Assume that no message longer than this will be used
#define MAX_SIZE 256

//The character array that can be read
static char stopwatch_msg[MAX_SIZE];

//The character array that can be written to
static char stopwatch_msg2[MAX_SIZE];

//File Operations structure to open, release and read device-driver
static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = stopwatch_read,
	.write = stopwatch_write,
	.open = stopwatch_open,
	.release = stopwatch_release
};

//Function to convert numeric value into output for 7 segment display
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

//Interrupt handler for the timer, interrupt is generated every millisecond
irq_handler_t irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{

    //Clear interrupt
    *(timer_ptr) = 0;


    int digit0, digit1, digit2, digit3, digit4, digit5;
    int temp;

    if (time > 0) {time--;}

    //Get seconds and miliseconds
    temp = time % 10000;

    //Wrap around to next minute
    if  (temp == 9999) { time = time  - 9999 + 5999;}

 //   set_time_display();
    digit0 = time %10; 		            //milliseconds first digit
    digit1 = (time %100)/10;		    //milliseconds second digit
    digit2 = (time %1000)/100;	        //seconds first digit
    digit3 = (time %10000)/1000;	    //seconds second digit
    digit4 = (time %100000)/10000;	    //minutes first digit
    digit5 = (time %1000000)/100000;	//minutes second digit

    //Assemble in minutes, seconds and milliseconds
    ms =  digit0 + (digit1*10);
    sec = digit2 + (digit3*10);
    min = digit4 + (digit5*10);

    //If Boolean disp is true output the stopwatch value to seven segment display
    if (disp){
        *(HEX_3_0_ptr) = hex(digit0)| (hex(digit1)<<8) | (hex(digit2) << 16) | (hex(digit3) << 24);
        *(HEX_4_5_ptr) = hex(digit4) | (hex(digit5) <<8);
    }

    //Copy current time value onto output variable
    sprintf(stopwatch_msg, "%02d:%02d:%02d",min,sec,ms);


   return (irq_handler_t) IRQ_HANDLED;
}


//Initialize the timmer, hex display and interrupt handler for timer
static int initialize_hextimer_handler(void)
{
    int value;

    //Generate a virtual address for the FPGA lightweight bridge
    LW_virtual = ioremap_nocache (LW_BRIDGE_BASE, LW_BRIDGE_SPAN);

    //Assign timer_ptr the address for Timer0
    timer_ptr = LW_virtual + TIMER0_BASE;

    //Initialize hex pointers
    HEX_3_0_ptr = LW_virtual + HEX3_HEX0_BASE;
    HEX_4_5_ptr = LW_virtual + HEX5_HEX4_BASE;

    //Set value of counter low 16-bits
    *(timer_ptr + 2) = 0x4240;
    //Set value of counter high 16 bits
    *(timer_ptr + 3) =0xF;
    //Set START, CONT and ITO bits as high
    *(timer_ptr + 1) = 0x7;

    //Register the interrupt handler.
    value = request_irq (INTERVAL_TIMER_IRQi, (irq_handler_t) irq_handler, IRQF_SHARED,"irq_handler", (void *) (irq_handler));

    return value;

}

//Initialize character device driver for the stopwatch
static int __init start_stopwatch(void)
{
	int err = 0;
	int value;
    time = 10000*min + 100*sec + ms;

	/* Get a device number. Get one minor number (0) */
	if ((err = alloc_chrdev_region (&stopwatch_no, 0, 1, DEVICE_NAME)) < 0) {
		printk (KERN_ERR "stopwatch: alloc_chrdev_region() failed with return value %d\n", err);
		return err;
	}

	// Allocate and initialize the character device
	stopwatch_cdev = cdev_alloc ();
	stopwatch_cdev->ops = &fops;
	stopwatch_cdev->owner = THIS_MODULE;

	// Add the character device to the kernel
	if ((err = cdev_add (stopwatch_cdev, stopwatch_no, 1)) < 0) {
		printk (KERN_ERR "stopwatch: cdev_add() failed with return value %d\n", err);
		return err;
	}

	stopwatch_class = class_create (THIS_MODULE, DEVICE_NAME);
	device_create (stopwatch_class, NULL, stopwatch_no, NULL, DEVICE_NAME );


	value = initialize_hextimer_handler();

	return 0;
}

//Destroy character device driver for stopwatch when done, free interrupt handler, turn off display
static void __exit stop_stopwatch(void)
{
	device_destroy (stopwatch_class, stopwatch_no);
	cdev_del (stopwatch_cdev);
	class_destroy (stopwatch_class);
	unregister_chrdev_region (stopwatch_no, 1);
	free_irq (INTERVAL_TIMER_IRQi, (void*) irq_handler);
	*HEX_3_0_ptr = 0; *HEX_4_5_ptr = 0;
	iounmap(LW_virtual);
}

//Called when a process opens stopwatch
static int stopwatch_open(struct inode *inode, struct file *file)
{
	return SUCCESS;
}

//Called when a process closes stopwatch
static int stopwatch_release(struct inode *inode, struct file *file)
{
	return SUCCESS;
}

//Called when a process reads from stopwatch
static ssize_t stopwatch_read(struct file *filp, char *buffer, size_t length, loff_t *offset)
{
	size_t bytes;


	bytes = strlen (stopwatch_msg) - (*offset);	// how many bytes not yet sent?
	bytes = bytes > length ? length : bytes;	// too much to send all at once?

	if (bytes)
		if (copy_to_user (buffer, &stopwatch_msg[*offset], bytes) != 0)
			printk (KERN_ERR "Error: copy_to_user unsuccessful");
	*offset = bytes;	// keep track of number of bytes sent to the user
	return bytes;
}

//Called when a process writes to stopwatch
static ssize_t stopwatch_write(struct file *filp, const char *buffer, size_t length, loff_t *offset)
{
	size_t bytes; //int num;
	bytes = length;
    char command[bytes];

	if (bytes > MAX_SIZE - 1)	// can copy all at once, or not?
		bytes = MAX_SIZE - 1;
	if (copy_from_user (stopwatch_msg2, buffer, bytes) != 0)
		printk (KERN_ERR "Error: copy_from_user unsuccessful");

    stopwatch_msg2[bytes] = '\0';
    sscanf (stopwatch_msg2, "%s", command);

    //Check user input and perform actions
    if (strcmp(command, "stop") == 0){*(timer_ptr+1) = 0xB; }
    else if (strcmp(command, "run") == 0){*(timer_ptr+1) = 0x7;}
    else if (strcmp(command, "disp") == 0){
        disp = true;
        *HEX_3_0_ptr = 0; *HEX_4_5_ptr = 0;
        int digit0,digit1,digit2,digit3,digit4,digit5;
        digit0 = ms %10; 		        //milliseconds first digit
        digit1 = (ms %100)/10;		    //milliseconds second digit
        digit2 = sec %10; 		        //seconds first digit
        digit3 = (sec %100)/10;		    //seconds second digit
        digit4 = min %10; 		        //min first digit
        digit5 = (min %100)/10;		    //min second digit
        *(HEX_3_0_ptr) = hex(digit0)| (hex(digit1)<<8) | (hex(digit2) << 16) | (hex(digit3) << 24);
        *(HEX_4_5_ptr) = hex(digit4) | (hex(digit5) <<8);
    }//set_time_display();}
    else if (strcmp(command, "nodisp") == 0){disp = false; *HEX_3_0_ptr = 0; *HEX_4_5_ptr = 0;}
    else if (sscanf(command, "%d:%d:%d", &min, &sec, &ms) == 3) {sprintf(stopwatch_msg, "%s", command);
	time = 10000*min + 100*sec + ms;}

	return bytes;
}

MODULE_LICENSE("GPL");
module_init (start_stopwatch);
module_exit (stop_stopwatch);
