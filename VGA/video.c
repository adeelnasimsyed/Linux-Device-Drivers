#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include "address_map_arm.h"


 // Declare global variables needed to use the pixel buffer
void *LW_virtual, *SDRAM_virtual; // used to access FPGA light-weight bridge
volatile int * pixel_ctrl_ptr, *character_ctrl_ptr; // virtual address of pixel buffer controller
int pixel_buffer, character_buffer; // used for virtual address of pixel buffer
int resolution_x, resolution_y; // VGA screen size

//The functions for the character device driver
static int video_open (struct inode *, struct file *);
static int video_release (struct inode *, struct file *);
static ssize_t video_read (struct file *, char *, size_t, loff_t *);
static ssize_t video_write (struct file *, const char *, size_t, loff_t *);

#define SUCCESS 0
#define DEVICE_NAME "video"
#define MAX_SIZE 256

static char video_msg[MAX_SIZE];
static char video_msg2[MAX_SIZE];
int back_buffer;

//Pointers for character device driver
static dev_t video_no = 0;
static struct cdev *video_cdev = NULL;
static struct class *video_class = NULL;

//File Operations structure to open, release, read and write device-driver
static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = video_read,
	.write = video_write,
	.open = video_open,
	.release = video_release
};


//Function to get screen specs
 void get_screen_specs(volatile int * pixel_ctrl_ptr)
 {

    resolution_x = (*(pixel_ctrl_ptr + 2) & 0xFFFF);
    resolution_y = ((*(pixel_ctrl_ptr + 2) >> 16 ) & 0xFFFF);


 }
//Function to clear screens
void clear_screen(void)
{
    int i,j;


    for(j = 0; j < resolution_y; j++){
        for(i = 0; i < resolution_x; i++){
            *(short int *) (back_buffer + (j << 10) + (i << 1)) = 0;
        }
    }

}
//Function to erase text
void erase_text(void)
{
    int char_resolution_x, char_resolution_y;
    int i,j;

    char_resolution_x = (*(character_ctrl_ptr + 2) & 0xFFFF);
    char_resolution_y = ((*(character_ctrl_ptr + 2) >> 16 ) & 0xFFFF);


    for(i = 0; i < char_resolution_x; i++){
        for(j = 0; j < char_resolution_y; j++){
            *(short int *) (character_buffer + (j << 7) + i) = ' ';
        }
    }

}

//Function to plot pixels
void plot_pixel(int x, int y, short int color)
{
        *(short int *) (back_buffer + (y << 10) + (x << 1)) = color;

}

//Function to plot charachters
void plot_chararcter(int x, int y, char * c, int length)
{

    char * t;
    int i = 0;

    for (t = c; *t != '\0'; t++){

       *(short int *) (character_buffer + (y << 7) + (x+i)) = *t;
        i++;
            }
}
//Function to swap numbers
void swap_(int * num1, int * num2){

    int temp;
    temp = *num1;
    *num1 = *num2;
    *num2 = temp;
}

//Function to compute absolute of number
int absolute(int x){

return (((x) > 0) ? (x) : -(x));}


//Function to draw line
void draw_line(int x0, int x1, int y0, int y1, short int color){

    int deltaX, deltaY, error, x, y, y_step;

    int is_steep = 0;

    if((absolute(y1 - y0)) > (absolute(x1 - x0))){
        is_steep = 1;}

    if (is_steep){

        swap_(&x0,&y0);
        swap_(&x1,&y1);
    }

    if (x0 > x1){

        swap_(&x0,&x1);
        swap_(&y0,&y1);

    }

    deltaX = x1 -x0;
    deltaY = absolute(y1 - y0);
    error = -(deltaX/2);

    y = y0;
    if (y0 < y1) { y_step = 1;}
    else {y_step = -1;}

    for (x = x0; x < (x1 + 1); x++){

        if (is_steep){ plot_pixel(y,x, color);}
        else {plot_pixel(x,y, color);}

        error += deltaY;

        if (error >= 0){
            y += y_step;
            error -= deltaX;

        }

    }


}

//Function to sync and swap buffers
void wait_for_vsync(volatile int *pixel_ctrl_ptr){

    volatile int status;

    *pixel_ctrl_ptr =1;
    status = *(pixel_ctrl_ptr + 3);

    while ((status & 0x01) != 0){

        status = *(pixel_ctrl_ptr +3);
    }

    if(*(pixel_ctrl_ptr + 1) == SDRAM_BASE){
    back_buffer = (int) SDRAM_virtual;
    }
    else{
    back_buffer = pixel_buffer;
    }
}


//Function to draw boxes
void draw_box(int x0, int x1, int y0, int y1, short int color){

    int i, j;

    get_screen_specs(pixel_ctrl_ptr);


    if (x0 < 2) { x0 = 2;}
    if (y0 < 2) { y0 = 2;}

    if (x1 < 2) { x1 = 2;}
    if (y1 < 2) { y1 = 2;}

    for (i = 0; i < 3; i++){

        for (j = 0; j < 3; j++){

            plot_pixel((x0-2+i), (y0-2+j), color);
            plot_pixel((x1-2+i), (y1-2+j), color);
        }
    }


}

 /* Code to initialize the video driver */
static int __init start_video(void)
{

    int err = 0;
    // initialize the dev_t, cdev, and class data structures

	if ((err = alloc_chrdev_region (&video_no, 0, 1, DEVICE_NAME)) < 0) {
		printk (KERN_ERR "video: alloc_chrdev_region() failed with return value %d\n", err);
		return err;
	}

	// Allocate and initialize the character device
	video_cdev = cdev_alloc ();
	video_cdev->ops = &fops;
	video_cdev->owner = THIS_MODULE;

	// Add the character device to the kernel
	if ((err = cdev_add (video_cdev, video_no, 1)) < 0) {
		printk (KERN_ERR "video: cdev_add() failed with return value %d\n", err);
		return err;
	}

	video_class = class_create (THIS_MODULE, DEVICE_NAME);
	device_create (video_class, NULL, video_no, NULL, DEVICE_NAME );


    // generate a virtual address for the FPGA lightweight bridge
    LW_virtual = ioremap_nocache (0xFF200000, 0x00005000);
    if (LW_virtual == 0)
    printk (KERN_ERR "Error: ioremap_nocache returned NULL\n");

    // Create virtual memory access to the pixel buffer controller
    pixel_ctrl_ptr = (unsigned int *) (LW_virtual + 0x00003020);
    character_ctrl_ptr = (unsigned int *) (LW_virtual + CHAR_BUF_CTRL_BASE);

    SDRAM_virtual = ioremap_nocache (SDRAM_BASE, SDRAM_SPAN);
    if (SDRAM_virtual == 0)
    printk (KERN_ERR "Error: ioremap_nocache returned NULL\n");
//
    character_buffer = (int)ioremap_nocache (FPGA_CHAR_BASE, FPGA_CHAR_SPAN);
    if (character_buffer == 0)
    printk (KERN_ERR "Error: ioremap_nocache returned NULL\n");


    get_screen_specs (pixel_ctrl_ptr); // determine X, Y screen size

    // Create virtual memory access to the pixel buffer
    pixel_buffer = (int) ioremap_nocache (0xC8000000, 0x0003FFFF);
    if (pixel_buffer == 0)
    printk (KERN_ERR "Error: ioremap_nocache returned NULL\n");

    back_buffer = SDRAM_virtual;
    /* Erase the pixel buffer */
    clear_screen();
    erase_text();
    return 0;
 }



static void __exit stop_video(void)
{
    /* unmap the physical-to-virtual mappings */
    iounmap (LW_virtual);
    iounmap (SDRAM_virtual);
    iounmap ((void *) character_buffer);
    iounmap ((void *) pixel_buffer);

    /* Remove the device from the kernel */
    device_destroy (video_class, video_no);
	cdev_del (video_cdev);
	class_destroy (video_class);
	unregister_chrdev_region (video_no, 1);
}

 static int video_open(struct inode *inode, struct file *file)
 {
    return SUCCESS;
 }
 static int video_release(struct inode *inode, struct file *file)
 {
    return SUCCESS;
 }
 static ssize_t video_read(struct file *filp, char *buffer, size_t length, loff_t *offset)
 {
	size_t bytes;

	get_screen_specs(pixel_ctrl_ptr);
	sprintf(video_msg, "%d %d\n", resolution_x, resolution_y);


	bytes = strlen (video_msg) - (*offset);	// how many bytes not yet sent?
	bytes = bytes > length ? length : bytes;	// too much to send all at once?

	if (bytes)
		if (copy_to_user (buffer, &video_msg[*offset], bytes) != 0)
			printk (KERN_ERR "Error: copy_to_user unsuccessful");
	*offset = bytes;	// keep track of number of bytes sent to the user
	return bytes;
}


 static ssize_t video_write(struct file *filp, const char *buffer, size_t length, loff_t *offset)
 {
 	size_t bytes;
	bytes = length;
    char command[bytes];
    char text_[bytes];
    int x1, y1, x2, y2;
    short int color;
    int text_length;

	if (bytes > MAX_SIZE - 1)	// can copy all at once, or not?
		bytes = MAX_SIZE - 1;
	if (copy_from_user (video_msg2, buffer, bytes) != 0)
		printk (KERN_ERR "Error: copy_from_user unsuccessful");

    video_msg2[bytes] = '\0';
    sscanf (video_msg2, "%s", command);

    //Check user input and perform actions
    if (strcmp(command, "clear") == 0){ clear_screen(); }

    else if (strcmp(command, "sync") == 0){wait_for_vsync(pixel_ctrl_ptr);}

    else if (sscanf(video_msg2, "pixel %d,%d %x", &x1, &y1, &color) == 3){
        plot_pixel(x1,y1,color); }

    else if (sscanf(video_msg2, "line %d,%d %d,%d %x", &x1, &y1, &x2, &y2,  &color) == 5){
        draw_line(x1,x2,y1,y2,color); }

    else if (sscanf(video_msg2, "box %d,%d %d,%d %x", &x1, &y1, &x2, &y2,  &color) == 5){
        draw_box(x1,x2,y1,y2,color); }

    else if (sscanf(video_msg2, "text %d,%d %s", &x1, &y1, text_) == 3){

       text_length = strlen(text_);
       plot_chararcter(x1,y1,text_,text_length);}

    else if (strcmp(command, "erase") == 0){erase_text();}



	return bytes;
  }

 MODULE_LICENSE("GPL");
 module_init (start_video);
 module_exit (stop_video);
