#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include "address_map_arm.h"
#include "ADXL345.h"

 // Declare global variables
int accel_buffer;
int r = 0;
int16_t mg_per_lsb = 31;
int16_t XYZ[3]={0};

volatile int *I2C0_ptr;	            // virtual address for I2C
volatile int *SYSMGR_ptr;           // virtual address for SYSMGR


//The functions for the character device driver
static int device_open (struct inode *, struct file *);
static int device_release (struct inode *, struct file *);
static ssize_t device_read (struct file *, char *, size_t, loff_t *);
static ssize_t device_write (struct file *, const char *, size_t, loff_t *);

#define SUCCESS 0
#define DEVICE_NAME "accel"
#define MAX_SIZE 25

static char accel_msg[MAX_SIZE];
static char accel_msg2[MAX_SIZE];

//Pointers for character device driver
static dev_t accel_no = 0;
static struct cdev *accel_cdev = NULL;
static struct class *accel_class = NULL;

//File Operations structure to open, release, read and write device-driver
static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release
};


void Pinmux_Config(void){
    // Set up pin muxing (in sysmgr) to connect ADXL345 wires to I2C0
    *(SYSMGR_ptr + SYSMGR_I2C0USEFPGA) = 0;
    *(SYSMGR_ptr + SYSMGR_GENERALIO7) = 1;
    *(SYSMGR_ptr + SYSMGR_GENERALIO8) = 1;
}


void I2C0_Init(void){

    // Abort any ongoing transmits and disable I2C0.
    *(I2C0_ptr + I2C0_ENABLE) = 2;

    // Wait until I2C0 is disabled
    while(((*(I2C0_ptr + I2C0_ENABLE_STATUS))&0x1) == 1){}

    // Configure the config reg with the desired setting (act as
    // a master, use 7bit addressing, fast mode (400kb/s)).
    *(I2C0_ptr + I2C0_CON) = 0x65;

    // Set target address (disable special commands, use 7bit addressing)
    *(I2C0_ptr + I2C0_TAR) = 0x53;

    // Set SCL high/low counts (Assuming default 100MHZ clock input to I2C0 Controller).
    // The minimum SCL high period is 0.6us, and the minimum SCL low period is 1.3us,
    // However, the combined period must be 2.5us or greater, so add 0.3us to each.
    *(I2C0_ptr + I2C0_FS_SCL_HCNT) = 60 + 30; // 0.6us + 0.3us
    *(I2C0_ptr + I2C0_FS_SCL_LCNT) = 130 + 30; // 1.3us + 0.3us

    // Enable the controller
    *(I2C0_ptr + I2C0_ENABLE) = 1;

    // Wait until controller is enabled
    while(((*(I2C0_ptr + I2C0_ENABLE_STATUS))&0x1) == 0){}

}


void ADXL345_REG_WRITE(uint8_t address, uint8_t value){

    // Send reg address (+0x400 to send START signal)
    *(I2C0_ptr + I2C0_DATA_CMD) = address + 0x400;

    // Send value
    *(I2C0_ptr + I2C0_DATA_CMD) = value;
}

void ADXL345_REG_READ(uint8_t address, uint8_t *value){

    // Send reg address (+0x400 to send START signal)
    *(I2C0_ptr + I2C0_DATA_CMD) = address + 0x400;

    // Send read signal
    *(I2C0_ptr + I2C0_DATA_CMD) = 0x100;

    // Read the response (first wait until RX buffer contains data)
    while (*(I2C0_ptr + I2C0_RXFLR) == 0){}
    *value = *(I2C0_ptr + I2C0_DATA_CMD);
}


void ADXL345_Init(void){

    // +- 16g range, 10 bit resolution
    ADXL345_REG_WRITE(ADXL345_REG_DATA_FORMAT, XL345_RANGE_16G | XL345_10BIT) ;

    // Output Data Rate: 12.5 Hz
    ADXL345_REG_WRITE(ADXL345_REG_BW_RATE,  XL345_RATE_12_5);

    // NOTE: The DATA_READY bit is not reliable. It is updated at a much higher rate than the Data Rate
    // Use the Activity and Inactivity interrupts.
    //----- Enabling interrupts -----//
    ADXL345_REG_WRITE(ADXL345_REG_THRESH_ACT, 0x04);	//activity threshold
    ADXL345_REG_WRITE(ADXL345_REG_THRESH_INACT, 0x02);	//inactivity threshold
    ADXL345_REG_WRITE(ADXL345_REG_TIME_INACT, 0x02);	//time for inactivity
    ADXL345_REG_WRITE(ADXL345_REG_ACT_INACT_CTL, 0xFF);	//Enables AC coupling for thresholds
    //ADXL345_REG_WRITE(ADXL345_REG_INT_ENABLE, XL345_SINGLETAP | XL345_DOUBLETAP);	//enable interrupts XL345_ACTIVITY | XL345_INACTIVITY
    //-------------------------------//

    // stop measure
    ADXL345_REG_WRITE(ADXL345_REG_POWER_CTL, XL345_STANDBY);

    // start measure
    ADXL345_REG_WRITE(ADXL345_REG_POWER_CTL, XL345_MEASURE);
}

bool ADXL345_WasActivityUpdated(void){
	bool bReady = false;
    uint8_t data8;

    ADXL345_REG_READ(ADXL345_REG_INT_SOURCE,&data8);
    if (data8 & XL345_ACTIVITY)
        bReady = true;

    return bReady;
}

// Return true if there is new data (checks DATA_READY bit).
bool ADXL345_IsDataReady(void){
    bool bReady = false;
    uint8_t data8;

    ADXL345_REG_READ(ADXL345_REG_INT_SOURCE,&data8);
    if (data8 & XL345_DATAREADY)
        bReady = true;
    return bReady;
}


// Read multiple consecutive internal registers
void ADXL345_REG_MULTI_READ(uint8_t address, uint8_t values[], uint8_t len){

    // Send reg address (+0x400 to send START signal)
    *(I2C0_ptr + I2C0_DATA_CMD) = address + 0x400;

    // Send read signal len times
    int i;
    for (i=0;i<len;i++)
        *(I2C0_ptr + I2C0_DATA_CMD) = 0x100;

    // Read the bytes
    int nth_byte=0;
    while (len){
        if ((*(I2C0_ptr + I2C0_RXFLR)) > 0){
            values[nth_byte] = *(I2C0_ptr + I2C0_DATA_CMD);
            nth_byte++;
            len--;
        }
    }
}

// Read acceleration data of all three axes
void ADXL345_XYZ_Read(int16_t szData16[3]){
    uint8_t szData8[6];

    ADXL345_REG_MULTI_READ(0x32, (uint8_t *)&szData8, sizeof(szData8));

    szData16[0] = (szData8[1] << 8) | szData8[0];
    szData16[1] = (szData8[3] << 8) | szData8[2];
    szData16[2] = (szData8[5] << 8) | szData8[4];
}


void ADXL345_Calibrate(void){

    int average_x = 0;
    int average_y = 0;
    int average_z = 0;
    int16_t XYZ[3];
    int8_t offset_x;
    int8_t offset_y;
    int8_t offset_z;

    // stop measure
    ADXL345_REG_WRITE(ADXL345_REG_POWER_CTL, XL345_STANDBY);

    // Get current offsets
    ADXL345_REG_READ(ADXL345_REG_OFSX, (uint8_t *)&offset_x);
    ADXL345_REG_READ(ADXL345_REG_OFSY, (uint8_t *)&offset_y);
    ADXL345_REG_READ(ADXL345_REG_OFSZ, (uint8_t *)&offset_z);

    // Use 100 hz rate for calibration. Save the current rate.
    uint8_t saved_bw;
    ADXL345_REG_READ(ADXL345_REG_BW_RATE, &saved_bw);
    ADXL345_REG_WRITE(ADXL345_REG_BW_RATE, XL345_RATE_100);

    // Use 16g range, full resolution. Save the current format.
    uint8_t saved_dataformat;
    ADXL345_REG_READ(ADXL345_REG_DATA_FORMAT, &saved_dataformat);
    ADXL345_REG_WRITE(ADXL345_REG_DATA_FORMAT, XL345_RANGE_16G | XL345_FULL_RESOLUTION);

    // start measure
    ADXL345_REG_WRITE(ADXL345_REG_POWER_CTL, XL345_MEASURE);

    // Get the average x,y,z accelerations over 32 samples (LSB 3.9 mg)
    int i = 0;
    while (i < 32){
		// Note: use DATA_READY here, can't use ACTIVITY because board is stationary.
        if (ADXL345_IsDataReady()){
            ADXL345_XYZ_Read(XYZ);
            average_x += XYZ[0];
            average_y += XYZ[1];
            average_z += XYZ[2];
            i++;
        }
    }
    average_x = ROUNDED_DIVISION(average_x, 32);
    average_y = ROUNDED_DIVISION(average_y, 32);
    average_z = ROUNDED_DIVISION(average_z, 32);

    // stop measure
    ADXL345_REG_WRITE(ADXL345_REG_POWER_CTL, XL345_STANDBY);

    // printf("Average X=%d, Y=%d, Z=%d\n", average_x, average_y, average_z);

    // Calculate the offsets (LSB 15.6 mg)
    offset_x += ROUNDED_DIVISION(0-average_x, 4);
    offset_y += ROUNDED_DIVISION(0-average_y, 4);
    offset_z += ROUNDED_DIVISION(256-average_z, 4);

    // printf("Calibration: offset_x: %d, offset_y: %d, offset_z: %d (LSB: 15.6 mg)\n",offset_x,offset_y,offset_z);

    // Set the offset registers
    ADXL345_REG_WRITE(ADXL345_REG_OFSX, offset_x);
    ADXL345_REG_WRITE(ADXL345_REG_OFSY, offset_y);
    ADXL345_REG_WRITE(ADXL345_REG_OFSZ, offset_z);

    // Restore original bw rate
    ADXL345_REG_WRITE(ADXL345_REG_BW_RATE, saved_bw);

    // Restore original data format
    ADXL345_REG_WRITE(ADXL345_REG_DATA_FORMAT, saved_dataformat);

    // start measure
    ADXL345_REG_WRITE(ADXL345_REG_POWER_CTL, XL345_MEASURE);
}

void ADXL345_TAP(void)
{
    //Tap threshold set at 3g
    ADXL345_REG_WRITE(ADXL345_REG_THRESH_TAP, 0x2F);

    //Tap duration set at 0.02s
    ADXL345_REG_WRITE(ADXL345_REG_DUR, 0x1F);

    //Tap latency set at 0.02s
    ADXL345_REG_WRITE(ADXL345_REG_LATENT, 0xF);

    //Tap window set at 0.3s
    ADXL345_REG_WRITE(ADXL345_REG_WINDOW, 0xEF);

    //Enable tap in axes
    ADXL345_REG_WRITE(ADXL345_REG_TAP_AXES, 0x1);


    ADXL345_REG_WRITE(ADXL345_REG_INT_ENABLE, XL345_SINGLETAP | XL345_DOUBLETAP);
}

 /* Code to initialize the accel driver */
static int __init start_accel(void)
{

    int err = 0;
    // initialize the dev_t, cdev, and class data structures

	if ((err = alloc_chrdev_region (&accel_no, 0, 1, DEVICE_NAME)) < 0) {
		printk (KERN_ERR "accel: alloc_chrdev_region() failed with return value %d\n", err);
		return err;
	}

	// Allocate and initialize the character device
	accel_cdev = cdev_alloc ();
	accel_cdev->ops = &fops;
	accel_cdev->owner = THIS_MODULE;

	// Add the character device to the kernel
	if ((err = cdev_add (accel_cdev, accel_no, 1)) < 0) {
		printk (KERN_ERR "accel: cdev_add() failed with return value %d\n", err);
		return err;
	}

	accel_class = class_create (THIS_MODULE, DEVICE_NAME);
	device_create (accel_class, NULL, accel_no, NULL, DEVICE_NAME );


    // generate a virtual addresses
    I2C0_ptr = ioremap_nocache (0xFFC04000, 0x00000100);
    SYSMGR_ptr = ioremap_nocache (0xFFD08000, 0x00000800);
    if ((I2C0_ptr == NULL) || (SYSMGR_ptr == NULL)){

        printk(KERN_ERR "Error: ioremap_nocache returned NULL\n");
        return (-1);

    }

    Pinmux_Config();
    I2C0_Init();
    ADXL345_Init();
    ADXL345_TAP();

    return 0;
 }



static void __exit stop_accel(void)
{
    /* unmap the physical-to-virtual mappings */
    iounmap ((void *) I2C0_ptr);
    iounmap ((void *) SYSMGR_ptr);

    /* Remove the device from the kernel */
    device_destroy (accel_class, accel_no);
	cdev_del (accel_cdev);
	class_destroy (accel_class);
	unregister_chrdev_region (accel_no, 1);
}

 static int device_open(struct inode *inode, struct file *file)
 {
    return SUCCESS;
 }
 static int device_release(struct inode *inode, struct file *file)
 {
    return SUCCESS;
 }
 static ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t *offset)
 {
	size_t bytes;
	uint8_t interrupt_source = 0;

    ADXL345_REG_READ(ADXL345_REG_INT_SOURCE, &interrupt_source);

    if (ADXL345_IsDataReady()){


        ADXL345_XYZ_Read(XYZ);

        //printk("%d\n", interrupt_source);
    }


    //ADXL345_XYZ_Read(XYZ);
	sprintf(accel_msg, "%2X %4d %4d %4d %2d\n", interrupt_source, XYZ[0], XYZ[1], XYZ[2], mg_per_lsb);
    accel_msg[strlen(accel_msg)] = '\0';

	bytes = strlen (accel_msg) - (*offset);	// how many bytes not yet sent?
	bytes = bytes > length ? length : bytes;	// too much to send all at once?

	if (bytes)
		if (copy_to_user (buffer, &accel_msg[*offset], bytes) != 0)
			printk (KERN_ERR "Error: copy_to_user unsuccessful");
	*offset = bytes;	// keep track of number of bytes sent to the user
	return bytes;
}


 static ssize_t device_write(struct file *filp, const char *buffer, size_t length, loff_t *offset)
 {
 	size_t bytes;
	bytes = length;
    char command[bytes];
    int format_, gravity_;
    int rate_;
    uint8_t devid;
    int range;

	if (bytes > MAX_SIZE - 1)	// can copy all at once, or not?
		bytes = MAX_SIZE - 1;
	if (copy_from_user (accel_msg2, buffer, bytes) != 0)
		printk (KERN_ERR "Error: copy_from_user unsuccessful");

    accel_msg2[bytes] = '\0';
    sscanf (accel_msg2, "%s", command);

    //Check user input and perform actions
    if (strcmp(command, "device") == 0){

        ADXL345_REG_READ(0x00, &devid);
        printk("The device ID is: 0x%X\n", devid);
    }

    else if (strcmp(command, "init") == 0){

         ADXL345_Init();
         printk("The device has been initialized\n");
    }

    else if (strcmp(command, "calibrate") == 0){

         ADXL345_Calibrate();
         printk("The device has been calibrated\n");
    }

    else if (sscanf(accel_msg2, "format %d %d", &format_, &gravity_) == 2){


        switch(gravity_){
            case 2:
                range = XL345_RANGE_2G;
               break;
            case 4:
                range = XL345_RANGE_4G;
               break;
            case 8:
                range = XL345_RANGE_8G;
               break;
            case 16:
                range = XL345_RANGE_16G;
               break;
        }


        if (format_ == 0){

            ADXL345_REG_WRITE(ADXL345_REG_DATA_FORMAT, XL345_10BIT | range);
            mg_per_lsb = (gravity_*2*1000)/(1024);
            printk("The device resolution is set to 10 bits\n");
        }

        else if (format_ == 1){

            ADXL345_REG_WRITE(ADXL345_REG_DATA_FORMAT, XL345_FULL_RESOLUTION | range);
            mg_per_lsb = 4;
            printk("The device resolution is set to full\n");
        }

    }

    else if (sscanf(accel_msg2, "rate %d", &rate_) == 1){

        if (rate_ == 12){

            ADXL345_REG_WRITE(ADXL345_REG_BW_RATE,  XL345_RATE_12_5);
        }

        else if (rate_ == 25){

            ADXL345_REG_WRITE(ADXL345_REG_BW_RATE,  XL345_RATE_25);
        }
        else if (rate_ == 50){

            ADXL345_REG_WRITE(ADXL345_REG_BW_RATE,  XL345_RATE_50);
        }
        else if (rate_ == 100){

            ADXL345_REG_WRITE(ADXL345_REG_BW_RATE,  XL345_RATE_100);
        }
        else if (rate_ == 200){

            ADXL345_REG_WRITE(ADXL345_REG_BW_RATE,  XL345_RATE_200);
        }
        else if (rate_ == 400){

            ADXL345_REG_WRITE(ADXL345_REG_BW_RATE,  XL345_RATE_400);
        }



    }


	return bytes;
}






 MODULE_LICENSE("GPL");
 module_init (start_accel);
 module_exit (stop_accel);
