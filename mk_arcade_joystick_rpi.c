/*
 *  Arcade Joystick Driver for RaspberryPi
 *
 *  Copyright (c) 2014 Matthieu Proucelle
 *
 *  Based on the gamecon driver by Vojtech Pavlik, and Markus Hiienkari
 */


/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <linux/ioport.h>
#include <asm/io.h>
#include <linux/version.h>

MODULE_AUTHOR("Matthieu Proucelle");
MODULE_DESCRIPTION("GPIO and MCP23017 and Multiplexer and 74HC165 Arcade Joystick Driver");
MODULE_LICENSE("GPL");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
#define HAVE_TIMER_SETUP
#endif


#define MK_MAX_DEVICES		9

// #ifdef RPI2
// #define PERI_BASE        0x3F000000
// #else
// #define PERI_BASE        0x20000000
// #endif
#define PERI_BASE           mk_bcm2708_peri_base

#define GPIO_BASE           (PERI_BASE + 0x200000) /* GPIO controller */

#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define GPIO_READ(g)  *(gpio + 13) &= (1<<(g))

#define GET_GPIO(g) (*(gpio+13) & (1<<g))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)
#define GPIO_CLR *(gpio+10)

#define BSC1_BASE		(PERI_BASE + 0x804000)


/*
 * MCP23017 Defines
 */
#define MPC23017_GPIOA_MODE		0x00
#define MPC23017_GPIOB_MODE		0x01
#define MPC23017_GPIOA_PULLUPS_MODE	0x0c
#define MPC23017_GPIOB_PULLUPS_MODE	0x0d
#define MPC23017_GPIOA_READ             0x12
#define MPC23017_GPIOB_READ             0x13

/*
 * Defines for I2C peripheral (aka BSC, or Broadcom Serial Controller)
 */

#define BSC1_C		*(bsc1 + 0x00)
#define BSC1_S		*(bsc1 + 0x01)
#define BSC1_DLEN	*(bsc1 + 0x02)
#define BSC1_A		*(bsc1 + 0x03)
#define BSC1_FIFO	*(bsc1 + 0x04)

#define BSC_C_I2CEN	(1 << 15)
#define BSC_C_INTR	(1 << 10)
#define BSC_C_INTT	(1 << 9)
#define BSC_C_INTD	(1 << 8)
#define BSC_C_ST	(1 << 7)
#define BSC_C_CLEAR	(1 << 4)
#define BSC_C_READ	1

#define START_READ	BSC_C_I2CEN|BSC_C_ST|BSC_C_CLEAR|BSC_C_READ
#define START_WRITE	BSC_C_I2CEN|BSC_C_ST

#define BSC_S_CLKT	(1 << 9)
#define BSC_S_ERR	(1 << 8)
#define BSC_S_RXF	(1 << 7)
#define BSC_S_TXE	(1 << 6)
#define BSC_S_RXD	(1 << 5)
#define BSC_S_TXD	(1 << 4)
#define BSC_S_RXR	(1 << 3)
#define BSC_S_TXW	(1 << 2)
#define BSC_S_DONE	(1 << 1)
#define BSC_S_TA	1

#define CLEAR_STATUS	BSC_S_CLKT|BSC_S_ERR|BSC_S_DONE

static volatile unsigned *gpio;
static volatile unsigned *bsc1;

struct mk_config {
    int args[MK_MAX_DEVICES];
    unsigned int nargs;
};

static struct mk_config mk_cfg __initdata;

module_param_array_named(map, mk_cfg.args, int, &(mk_cfg.nargs), 0);
MODULE_PARM_DESC(map, "Enable or disable GPIO, MCP23017, TFT and Custom Arcade Joystick");

struct gpio_config {
    int mk_arcade_gpio_maps_custom[12];
    unsigned int nargs;
};

static struct gpio_config gpio_cfg __initdata;

module_param_array_named(gpio, gpio_cfg.mk_arcade_gpio_maps_custom, int, &(gpio_cfg.nargs), 0);
MODULE_PARM_DESC(gpio, "Numbers of custom GPIO for Arcade Joystick");

struct ext_config {
    int args[8];
    unsigned int nargs;
};

static struct ext_config ext_cfg __initdata;

module_param_array_named(ext, ext_cfg.args, int, &(ext_cfg.nargs), 0);
MODULE_PARM_DESC(ext, "Extend config for Arcade Joystick");

enum mk_type {
    MK_NONE = 0,
    MK_ARCADE_GPIO,
    MK_ARCADE_GPIO_BPLUS,
    MK_ARCADE_MCP23017,
    MK_ARCADE_GPIO_TFT,
    MK_ARCADE_GPIO_CUSTOM,
    MK_ARCADE_GPIO_MULTIPLEXER,
    MK_ARCADE_GPIO_74HC165,
    MK_MAX
};

#define MK_REFRESH_TIME	HZ/100

struct mk_pad {
    struct input_dev *dev;
    enum mk_type type;
    char phys[32];
    int mcp23017addr;
    int gpio_maps[12];
    int start_offs;
    int button_count;
};

struct mk_nin_gpio {
    unsigned pad_id;
    unsigned cmd_setinputs;
    unsigned cmd_setoutputs;
    unsigned valid_bits;
    unsigned request;
    unsigned request_len;
    unsigned response_len;
    unsigned response_bufsize;
};

struct mk {
    struct mk_pad pads[MK_MAX_DEVICES];
    struct timer_list timer;
    int pad_count[MK_MAX];
    int used;
    struct mutex mutex;
};

struct mk_subdev {
    unsigned int idx;
};

static struct mk *mk_base;

static const int mk_data_size = 16;

static const int mk_max_arcade_buttons = 12;
static const int mk_max_mcp_arcade_buttons = 16;

// Map of the gpios :                     up, down, left, right, start, select, a,  b,  tr, y,  x,  tl
static const int mk_arcade_gpio_maps[] = { 4,  17,    27,  22,    10,    9,      25, 24, 23, 18, 15, 14 };
// 2nd joystick on the b+ GPIOS                 up, down, left, right, start, select, a,  b,  tr, y,  x,  tl
static const int mk_arcade_gpio_maps_bplus[] = { 11, 5,    6,    13,    19,    26,     21, 20, 16, 12, 7,  8 };
// Map of the mcp23017 on GPIOA            up, down, left, right, start, select, a,	 b
static const int mk_arcade_gpioa_maps[] = { 0,  1,    2,    3,     4,     5,	6,	 7 };

// Map of the mcp23017 on GPIOB            tr, y, x, tl, c, tr2, z, tl2
static const int mk_arcade_gpiob_maps[] = { 0, 1, 2,  3, 4, 5,   6, 7 };

// Map joystick on the b+ GPIOS with TFT      up, down, left, right, start, select, a,  b,  tr, y,  x,  tl
static const int mk_arcade_gpio_maps_tft[] = { 21, 13,    26,    19,    5,    6,     22, 4, 20, 17, 27,  16 };

static const short mk_arcade_gpio_btn[] = {
	BTN_START, BTN_SELECT, BTN_A, BTN_B, BTN_TR, BTN_Y, BTN_X, BTN_TL, BTN_C, BTN_TR2, BTN_Z, BTN_TL2
};

static const char *mk_names[] = {
    NULL, "GPIO Controller 1", "GPIO Controller 2", "MCP23017 Controller", "GPIO Controller 1" , "GPIO Controller 1", "Multiplexer Controller", "74HC165 Controller"
};

/* GPIO UTILS */
static void setGpioPullUps(int pullUps) {
    *(gpio + 37) = 0x02;
    udelay(10);
    *(gpio + 38) = pullUps;
    udelay(10);
    *(gpio + 37) = 0x00;
    *(gpio + 38) = 0x00;
}

static void setGpioAsInput(int gpioNum) {
    INP_GPIO(gpioNum);
}

static void setGpioAsOutput(int gpioNum) {
    OUT_GPIO(gpioNum);
}

static int getPullUpMask(int gpioMap[], int count){
    int mask = 0x0000000;
    int i;
    for(i = 0; i < count; i++) {
        if(gpioMap[i] != -1){   // to avoid unused pins
            int pin_mask  = 1<<gpioMap[i];
            mask = mask | pin_mask;
        }
    }
    return mask;
}

static void putGpioValue(int gpiono, int onoff) {
    if (onoff) 
        GPIO_SET = (1 << gpiono); 
    else
        GPIO_CLR = (1 << gpiono);
}

/* I2C UTILS */
static void i2c_init(void) {
    INP_GPIO(2);
    SET_GPIO_ALT(2, 0);
    INP_GPIO(3);
    SET_GPIO_ALT(3, 0);
}

static void wait_i2c_done(void) {
    while ((!((BSC1_S) & BSC_S_DONE))) {
        udelay(100);
    }
}

// Function to write data to an I2C device via the FIFO.  This doesn't refill the FIFO, so writes are limited to 16 bytes
// including the register address. len specifies the number of bytes in the buffer.

static void i2c_write(char dev_addr, char reg_addr, char *buf, unsigned short len) {

    int idx;

    BSC1_A = dev_addr;
    BSC1_DLEN = len + 1; // one byte for the register address, plus the buffer length

    BSC1_FIFO = reg_addr; // start register address
    for (idx = 0; idx < len; idx++)
        BSC1_FIFO = buf[idx];

    BSC1_S = CLEAR_STATUS; // Reset status bits (see #define)
    BSC1_C = START_WRITE; // Start Write (see #define)

    wait_i2c_done();

}

// Function to read a number of bytes into a  buffer from the FIFO of the I2C controller

static void i2c_read(char dev_addr, char reg_addr, char *buf, unsigned short len) {
    unsigned short bufidx;

    i2c_write(dev_addr, reg_addr, NULL, 0);

    bufidx = 0;

    memset(buf, 0, len); // clear the buffer

    BSC1_DLEN = len;
    BSC1_S = CLEAR_STATUS; // Reset status bits (see #define)
    BSC1_C = START_READ; // Start Read after clearing FIFO (see #define)

    do {
        // Wait for some data to appear in the FIFO
        while ((BSC1_S & BSC_S_TA) && !(BSC1_S & BSC_S_RXD));

        // Consume the FIFO
        while ((BSC1_S & BSC_S_RXD) && (bufidx < len)) {
            buf[bufidx++] = BSC1_FIFO;
        }
    } while ((!(BSC1_S & BSC_S_DONE)));
}

/*  ------------------------------------------------------------------------------- */

static void mk_mcp23017_read_packet(struct mk_pad * pad, unsigned char *data) {
    int i;
    char resultA, resultB;
    i2c_read(pad->mcp23017addr, MPC23017_GPIOA_READ, &resultA, 1);
    i2c_read(pad->mcp23017addr, MPC23017_GPIOB_READ, &resultB, 1);

    // read direction
    for (i = 0; i < 4; i++) {
        data[i] = !((resultA >> mk_arcade_gpioa_maps[i]) & 0x1);
    }
    // read buttons on gpioa
    for (i = 4; i < 8; i++) {
        data[i] = !((resultA >> mk_arcade_gpioa_maps[i]) & 0x1);
    }
    // read buttons on gpiob
    for (i = 8; i < 16; i++) {
        data[i] = !((resultB >> (mk_arcade_gpiob_maps[i-8])) & 0x1);
    }
}

static void mk_gpio_read_packet(struct mk_pad * pad, unsigned char *data) {
    int i;

    for (i = 0; i < mk_max_arcade_buttons; i++) {
        if(pad->gpio_maps[i] != -1){    // to avoid unused buttons
            int read = GPIO_READ(pad->gpio_maps[i]);
            if (read == 0) data[i] = 1;
            else data[i] = 0;
        }else data[i] = 0;
    }
}

static void mk_multiplexer_read_packet(struct mk_pad * pad, unsigned char *data) {
    int i, value;
    int addr0 = pad->gpio_maps[0];
    int addr1 = pad->gpio_maps[1];
    int addr2 = pad->gpio_maps[2];
    int addr3 = pad->gpio_maps[3];
    int readp = pad->gpio_maps[4];
    int startoffs = pad->start_offs;
    int loopcount = pad->button_count;

    for (i = 0; i < loopcount; i++) {
        int addr = i + startoffs;
        putGpioValue(addr0, addr & 1);
        putGpioValue(addr1, (addr >> 1) & 1);
        putGpioValue(addr2, (addr >> 2) & 1);
        putGpioValue(addr3, (addr >> 3) & 1);
        udelay(5);
        value = GET_GPIO(readp);
        data[i] = (value == 0)? 1 : 0;
    }
    for (i = loopcount; i < mk_max_arcade_buttons; i++) {
        data[i] = (value == 0)? 1 : 0;
    }
}

static void mk_74hc165_read_packet(struct mk_pad * pad, unsigned char *data) {
    int i, idx, value;
    int ld = pad->gpio_maps[0];
    int cl = pad->gpio_maps[1];
    int readp = pad->gpio_maps[2];
    int startoffs = pad->start_offs;
    int loopcount = pad->button_count;

    putGpioValue(ld, 0);
    udelay(5);
    putGpioValue(ld, 1);
    idx = 0;
    for (i = 0; i < startoffs; i++) {
        value = GET_GPIO(readp);
    }
    for (i = 0; i < loopcount; i++) {
        value = GET_GPIO(readp);
        data[i] = (value == 0)? 1 : 0;
    }
    for (i = loopcount; i < mk_max_arcade_buttons; i++) {
        data[i] = (value == 0)? 1 : 0;
    }
}

static void mk_input_report(struct mk_pad * pad, unsigned char * data) {
    struct input_dev * dev = pad->dev;
    int j;
    input_report_abs(dev, ABS_Y, !data[0]-!data[1]);
    input_report_abs(dev, ABS_X, !data[2]-!data[3]);
	if (pad->type == MK_ARCADE_MCP23017) {	// check if MCP23017 and extend with 4.
		for (j = 4; j < (mk_max_mcp_arcade_buttons); j++) {
			input_report_key(dev, mk_arcade_gpio_btn[j - 4], data[j]);
		}
	}
	else {
		for (j = 4; j < mk_max_arcade_buttons; j++) {
			input_report_key(dev, mk_arcade_gpio_btn[j - 4], data[j]);
		}
	}
    input_sync(dev);
}

static void mk_process_packet(struct mk *mk) {

    unsigned char data[mk_data_size];
    struct mk_pad *pad;
    int i;

    for (i = 0; i < MK_MAX_DEVICES; i++) {
        pad = &mk->pads[i];
        if (pad->type == MK_ARCADE_GPIO || pad->type == MK_ARCADE_GPIO_BPLUS || pad->type == MK_ARCADE_GPIO_TFT || pad->type == MK_ARCADE_GPIO_CUSTOM) {
            mk_gpio_read_packet(pad, data);
            mk_input_report(pad, data);
        } else if (pad->type == MK_ARCADE_MCP23017) {
            mk_mcp23017_read_packet(pad, data);
            mk_input_report(pad, data);
        } else if (pad->type == MK_ARCADE_GPIO_MULTIPLEXER) {
            mk_multiplexer_read_packet(pad, data);
            mk_input_report(pad, data);
        } else if (pad->type == MK_ARCADE_GPIO_74HC165) {
            mk_74hc165_read_packet(pad, data);
            mk_input_report(pad, data);
        }
    }

}

/*
 * mk_timer() initiates reads of console pads data.
 */

#ifdef HAVE_TIMER_SETUP
static void mk_timer(struct timer_list *t) {
    struct mk *mk = from_timer(mk, t, timer);
#else
static void mk_timer(unsigned long private) {
    struct mk *mk = (void *) private;
#endif
    mk_process_packet(mk);
    mod_timer(&mk->timer, jiffies + MK_REFRESH_TIME);
}

static int mk_open(struct input_dev *dev) {
    struct mk *mk = input_get_drvdata(dev);
    int err;

    err = mutex_lock_interruptible(&mk->mutex);
    if (err)
        return err;

    if (!mk->used++)
        mod_timer(&mk->timer, jiffies + MK_REFRESH_TIME);

    mutex_unlock(&mk->mutex);
    return 0;
}

static void mk_close(struct input_dev *dev) {
    struct mk *mk = input_get_drvdata(dev);

    mutex_lock(&mk->mutex);
    if (!--mk->used) {
        del_timer_sync(&mk->timer);
    }
    mutex_unlock(&mk->mutex);
}

static int __init mk_setup_pad(struct mk *mk, int idx, int pad_type_arg) {
    struct mk_pad *pad = &mk->pads[idx];
    struct input_dev *input_dev;
    int i, pad_type;
    int err;
    char FF = 0xFF;
    pr_err("pad type : %d\n",pad_type_arg);

    if (pad_type_arg >= MK_MAX) {
        pad_type = MK_ARCADE_MCP23017;
    } else {
        pad_type = pad_type_arg;
    }

    if (pad_type < 1 || pad_type >= MK_MAX) {
        pr_err("Pad type %d unknown\n", pad_type);
        return -EINVAL;
    }

    if (pad_type == MK_ARCADE_GPIO_CUSTOM) {
        // if the device is custom, be sure to get correct pins
        if (gpio_cfg.nargs < 1) {
            pr_err("Custom device needs gpio argument\n");
            return -EINVAL;
        } else if(gpio_cfg.nargs != 12){
             pr_err("Invalid gpio argument\n", pad_type);
             return -EINVAL;
        }
    } else if (pad_type == MK_ARCADE_GPIO_MULTIPLEXER) {
        // if the device is multiplexer, be sure to get correct pins
        if (gpio_cfg.nargs < 1) {
            pr_err("Multiplexer device needs gpio argument\n");
            return -EINVAL;
        } else if(gpio_cfg.nargs != 5){
             pr_err("Invalid gpio argument\n", pad_type);
             return -EINVAL;
        }
    } else if (pad_type == MK_ARCADE_GPIO_74HC165) {
        // if the device is 74HC165, be sure to get correct pins
        if (gpio_cfg.nargs < 1) {
            pr_err("74HC165 device needs gpio argument\n");
            return -EINVAL;
        } else if(gpio_cfg.nargs != 3){
             pr_err("Invalid gpio argument\n", pad_type);
             return -EINVAL;
        }
    }

    pr_err("pad type : %d\n",pad_type);
    pad->dev = input_dev = input_allocate_device();
    if (!input_dev) {
        pr_err("Not enough memory for input device\n");
        return -ENOMEM;
    }

    pad->type = pad_type;
    pad->mcp23017addr = pad_type_arg;
    snprintf(pad->phys, sizeof (pad->phys),
            "input%d", idx);

    input_dev->name = mk_names[pad_type];
    input_dev->phys = pad->phys;
    input_dev->id.bustype = BUS_PARPORT;
    input_dev->id.vendor = 0x0001;
    input_dev->id.product = pad_type;
    input_dev->id.version = 0x0100;

    input_set_drvdata(input_dev, mk);

    input_dev->open = mk_open;
    input_dev->close = mk_close;

    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

    for (i = 0; i < 2; i++)
        input_set_abs_params(input_dev, ABS_X + i, -1, 1, 0, 0);
	if (pad_type != MK_ARCADE_MCP23017)
	{
		for (i = 0; i < mk_max_arcade_buttons; i++)
			__set_bit(mk_arcade_gpio_btn[i], input_dev->keybit);
	}
	else { //Checking for MCP23017 so it gets 4 more buttons registered to it.
		for (i = 0; i < mk_max_mcp_arcade_buttons; i++)
			__set_bit(mk_arcade_gpio_btn[i], input_dev->keybit);
	}

    mk->pad_count[pad_type]++;

    // asign gpio pins
    switch (pad_type) {
        case MK_ARCADE_GPIO:
            memcpy(pad->gpio_maps, mk_arcade_gpio_maps, 12 *sizeof(int));
            break;
        case MK_ARCADE_GPIO_BPLUS:
            memcpy(pad->gpio_maps, mk_arcade_gpio_maps_bplus, 12 *sizeof(int));
            break;
        case MK_ARCADE_GPIO_TFT:
            memcpy(pad->gpio_maps, mk_arcade_gpio_maps_tft, 12 *sizeof(int));
            break;
        case MK_ARCADE_GPIO_CUSTOM:
            memcpy(pad->gpio_maps, gpio_cfg.mk_arcade_gpio_maps_custom, 12 *sizeof(int));
            break;
        case MK_ARCADE_MCP23017:
            // nothing to asign if MCP23017 is used
            break;
        case MK_ARCADE_GPIO_MULTIPLEXER:
            memcpy(pad->gpio_maps, gpio_cfg.mk_arcade_gpio_maps_custom, 5 *sizeof(int));
            pad->start_offs = 0;
            pad->button_count = mk_max_arcade_buttons;
            if (ext_cfg.nargs >= 1) {
                pad->start_offs = ext_cfg.args[0];
                if (ext_cfg.nargs >= 2) {
                    pad->button_count = ext_cfg.args[1];
                }
            }
            break;
        case MK_ARCADE_GPIO_74HC165:
            memcpy(pad->gpio_maps, gpio_cfg.mk_arcade_gpio_maps_custom, 3 *sizeof(int));
            pad->start_offs = 0;
            pad->button_count = mk_max_arcade_buttons;
            if (ext_cfg.nargs >= 1) {
                pad->start_offs = ext_cfg.args[0];
                if (ext_cfg.nargs >= 2) {
                    pad->button_count = ext_cfg.args[1];
                }
            }
            break;
    }

    // initialize gpio if not MCP23017, else initialize i2c
    if(pad_type == MK_ARCADE_MCP23017){
        i2c_init();
        udelay(1000);
        // Put all GPIOA inputs on MCP23017 in INPUT mode
        i2c_write(pad->mcp23017addr, MPC23017_GPIOA_MODE, &FF, 1);
        udelay(1000);
        // Put all inputs on MCP23017 in pullup mode
        i2c_write(pad->mcp23017addr, MPC23017_GPIOA_PULLUPS_MODE, &FF, 1);
        udelay(1000);
        // Put all GPIOB inputs on MCP23017 in INPUT mode
        i2c_write(pad->mcp23017addr, MPC23017_GPIOB_MODE, &FF, 1);
        udelay(1000);
        // Put all inputs on MCP23017 in pullup mode
        i2c_write(pad->mcp23017addr, MPC23017_GPIOB_PULLUPS_MODE, &FF, 1);
        udelay(1000);
        // Put all inputs on MCP23017 in pullup mode a second time
        // Known bug : if you remove this line, you will not have pullups on GPIOB 
        i2c_write(pad->mcp23017addr, MPC23017_GPIOB_PULLUPS_MODE, &FF, 1);
        udelay(1000);
    } else if(pad_type == MK_ARCADE_GPIO_MULTIPLEXER) {
        for (i = 0; i < 5; i++) {
            printk("GPIO = %d\n", pad->gpio_maps[i]);
        }
        setGpioAsOutput(pad->gpio_maps[0]);
        setGpioAsOutput(pad->gpio_maps[1]);
        setGpioAsOutput(pad->gpio_maps[2]);
        setGpioAsOutput(pad->gpio_maps[3]);
        setGpioAsInput(pad->gpio_maps[4]);
        setGpioPullUps(getPullUpMask(&pad->gpio_maps[4], 1));
        printk("GPIO configured for pad%d\n", idx);
    } else if(pad_type == MK_ARCADE_GPIO_74HC165) {
        for (i = 0; i < 3; i++) {
            printk("GPIO = %d\n", pad->gpio_maps[i]);
        }
        setGpioAsOutput(pad->gpio_maps[0]);
        setGpioAsOutput(pad->gpio_maps[1]);
        setGpioAsInput(pad->gpio_maps[2]);
        setGpioPullUps(getPullUpMask(&pad->gpio_maps[2], 1));
        printk("GPIO configured for pad%d\n", idx);
    } else {
        for (i = 0; i < mk_max_arcade_buttons; i++) {
            printk("GPIO = %d\n", pad->gpio_maps[i]);
            if(pad->gpio_maps[i] != -1){    // to avoid unused buttons
                 setGpioAsInput(pad->gpio_maps[i]);
            }                
        }
        setGpioPullUps(getPullUpMask(pad->gpio_maps, 12));
        printk("GPIO configured for pad%d\n", idx);
	}

    err = input_register_device(pad->dev);
    if (err)
        goto err_free_dev;

    return 0;

err_free_dev:
    input_free_device(pad->dev);
    pad->dev = NULL;
    return err;
}

static struct mk __init *mk_probe(int *pads, int n_pads) {
    struct mk *mk;
    int i;
    int count = 0;
    int err;

    mk = kzalloc(sizeof (struct mk), GFP_KERNEL);
    if (!mk) {
        pr_err("Not enough memory\n");
        err = -ENOMEM;
        goto err_out;
    }

    mutex_init(&mk->mutex);
#ifdef HAVE_TIMER_SETUP
    timer_setup(&mk->timer, mk_timer, 0);
#else
    setup_timer(&mk->timer, mk_timer, (long) mk);
#endif

    for (i = 0; i < n_pads && i < MK_MAX_DEVICES; i++) {
        if (!pads[i])
            continue;

        err = mk_setup_pad(mk, i, pads[i]);
        if (err)
            goto err_unreg_devs;

        count++;
    }

    if (count == 0) {
        pr_err("No valid devices specified\n");
        err = -EINVAL;
        goto err_free_mk;
    }

    return mk;

err_unreg_devs:
    while (--i >= 0)
        if (mk->pads[i].dev)
            input_unregister_device(mk->pads[i].dev);
err_free_mk:
    kfree(mk);
err_out:
    return ERR_PTR(err);
}

static void mk_remove(struct mk *mk) {
    int i;

    for (i = 0; i < MK_MAX_DEVICES; i++)
        if (mk->pads[i].dev)
            input_unregister_device(mk->pads[i].dev);
    kfree(mk);
}

static int __init mk_init(void) {
    /* Set up gpio pointer for direct register access */
    if ((gpio = ioremap(GPIO_BASE, 0xB0)) == NULL) {
        pr_err("io remap failed\n");
        return -EBUSY;
    }
    /* Set up i2c pointer for direct register access */
    if ((bsc1 = ioremap(BSC1_BASE, 0xB0)) == NULL) {
        pr_err("io remap failed\n");
        return -EBUSY;
    }
    if (mk_cfg.nargs < 1) {
        pr_err("at least one device must be specified\n");
        return -EINVAL;
    } else {
        mk_base = mk_probe(mk_cfg.args, mk_cfg.nargs);
        if (IS_ERR(mk_base))
            return -ENODEV;
    }
    return 0;
}

static void __exit mk_exit(void) {
    if (mk_base)
        mk_remove(mk_base);

    iounmap(gpio);
    iounmap(bsc1);
}

module_init(mk_init);
module_exit(mk_exit);
