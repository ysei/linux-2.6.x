/*
 * File:         drivers/media/video/blackfin/blackfin_cam.c
 * Based on:
 * Author:       Michael Benjamin
 *
 * Created:
 * Description:  V4L driver for Blackfin
 *
 * Rev:          $Id: mt9m001.c 2792 2007-03-02 16:21:01Z hennerich $
 *
 * Modified:
 *               Copyright 2004-2006 Analog Devices Inc.
 *
 * Bugs:         Enter bugs at http://blackfin.uclinux.org/
 *
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
 * along with this program; if not, see the file COPYING, or write
 * to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>

#include <media/v4l2-dev.h>

#include <asm/blackfin.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/dma.h>
#include <asm/cacheflush.h>
#include <asm/uaccess.h>
#include <asm/gpio.h>

#if defined(CONFIG_BF537)
# define  uCAM_STANDBY  GPIO_PG11
# define  uCAM_LEDS     GPIO_PG8
# define  uCAM_TRIGGER  GPIO_PG13
#endif

#if defined(CONFIG_BF533)
# define  uCAM_STANDBY  GPIO_8
# define  uCAM_LEDS     GPIO_11
# define  uCAM_TRIGGER  GPIO_6
# define  uCAM_FS3      GPIO_3
#endif

#define  uCAM_NUM_BUFS 2
#define  VID_HARDWARE_UCAM  13	/* experimental */
#define  I2C_DRIVERID_UCAM  81	/* experimental (next avail. in i2c-id.h) */
#define NO_TRIGGER  16

#ifdef CONFIG_VS6524
#include "vs6524.h"
#endif


#ifdef USE_GPIO
#define GPIO_SET_VALUE(x,y) gpio_set_value(x,y)
#else
#define GPIO_SET_VALUE(x,y) do{}while(0)
#endif

struct uCam_device_t;

#define  MAX_BUFFER_SIZE (MAX_FRAME_WIDTH * MAX_FRAME_HEIGHT * DEFAULT_DEPTH/8)

static unsigned char *top_buffer;	/* TOP Video Buffer */
static dma_addr_t dma_handle;

static unsigned int global_gain = 127;
static unsigned int debug = 0;
static unsigned int perfnum = 0;
static unsigned int force_palette = DEFAULT_FORMAT;

struct i2c_client *i2c_global_client = NULL;	/* just needed for proc ... */
struct mt9m001_data {
	struct i2c_client client;
	struct uCam_device_t *uCam_dev;
	unsigned char reg[128];
	int input;
	int enable;
	int bright;
	int contrast;
	int hue;
	int sat;
};
static const char sensor_name[] = SENSOR_NAME;
struct i2c_adapter *adapter;
static u16 normal_i2c[] = {
	I2C_SENSOR_ID >> 1,
	(I2C_SENSOR_ID >> 1) + 1,
	I2C_CLIENT_END
};

I2C_CLIENT_INSMOD;
static struct i2c_driver mt9m001_driver;

struct ppi_device_t {
	struct uCam_device_t *uCam_dev;
	int opened;
	int nonblock;
	unsigned short irqnum;
	unsigned short done;
	unsigned short dma_config;
	unsigned short pixel_per_line;
	unsigned short lines_per_frame;
	unsigned short bpp;
	unsigned short ppi_control;
	unsigned short ppi_status;
	unsigned short ppi_delay;
	unsigned short ppi_trigger_gpio;
	struct fasync_struct *fasyc;
	wait_queue_head_t *rx_avail;
};

struct uCam_buffer {
	unsigned char *data;
	wait_queue_head_t wq;
	volatile int state;
	unsigned int scyc;	/* < start cycle for frame  */
	unsigned int ecyc;	/* < end cycle for frame    */
	unsigned long stime;	/* < start time for frame   */
	unsigned long etime;	/* < end time for frame     */
};

/*
 * States for each frame buffer.
 */
enum {
	FRAME_UNUSED = 0,	/* < Unused                           */
	FRAME_READY = 1,	/* < Ready to start grabbing          */
	FRAME_GRABBING = 2,	/* < Grabbing the frame               */
	FRAME_DONE = 3,		/* < Grabbing done, frame not synced  */
	FRAME_ERROR = 4,	/* < Error                            */
};

struct uCam_device_t {
	int frame_count;
	struct video_device *videodev;
	struct ppi_device_t *ppidev;
	struct i2c_client *client;
	int user;
	char name[32];
	int width;
	int height;
	size_t size;
	struct timeval *stv;
	struct timeval *etv;
	struct uCam_buffer buffer[uCAM_NUM_BUFS];
	struct uCam_buffer *next_buf;
	struct uCam_buffer *dma_buf;
	struct uCam_buffer *ready_buf;
	spinlock_t lock;
};
static struct video_device uCam_template;
struct uCam_device_t *uCam_dev;
static DECLARE_WAIT_QUEUE_HEAD(uCam_waitqueue);

static inline unsigned int cycles(void)
{
	int ret;
	__asm__ __volatile__("%0 = CYCLES;\n\t":"=d"(ret));
	return ret;
}

static inline int
default_palette(int palette)
{
	switch (palette) {
	case 0:  return VIDEO_PALETTE_GREY;
	case 1:  return VIDEO_PALETTE_RGB565;
	case 2:  return VIDEO_PALETTE_YUV422;
	case 3:  return VIDEO_PALETTE_UYVY;

	default:
		return VIDEO_PALETTE_RGB565;
	}
}

void ucam_reg_reset(struct ppi_device_t *pdev)
{
	pr_debug("ucam_reg_reset:\n");

	bfin_clear_PPI_STATUS();
	bfin_write_PPI_CONTROL(pdev->ppi_control & ~PORT_EN);
	bfin_write_PPI_DELAY(pdev->ppi_delay);
	bfin_write_PPI_COUNT(pdev->pixel_per_line - 1);
	bfin_write_PPI_FRAME(pdev->lines_per_frame);
}


static size_t ppi2dma(struct ppi_device_t *ppidev, char *buf, size_t count)
{
	int ierr;

	ppidev->done = 0;

	if (count <= 0)
		return 0;

	pr_debug("ppi2dma: reading %zi bytes (%dx%d) into [0x%p]\n",
		 count, ppidev->uCam_dev->width, ppidev->uCam_dev->height, buf);

	set_dma_start_addr(CH_PPI, (u_long) buf);

	enable_dma(CH_PPI);

	/* Enable PPI  */
	bfin_write_PPI_CONTROL(bfin_read_PPI_CONTROL() | PORT_EN);

	if (ppidev->nonblock) {
		pr_debug("ppi2dma: awaiting IRQ...\n");
		return -EAGAIN;
	} else {
		printk("ppi2dma: PPI wait_event_interruptible\n");
		ierr =
		    wait_event_interruptible(*(ppidev->rx_avail), ppidev->done);
		if (ierr) {
			/* waiting is broken by a signal */
			pr_debug("PPI wait_event_interruptible ierr\n");
			return ierr;
		}
		pr_debug("ppi2dma: PPI wait_event_interruptible done\n");
	}

	disable_dma(CH_PPI);

	pr_debug("ppi2dma: done read in %zi bytes for [0x%p-0x%p]\n", count,
		 buf, (buf + count));

	return count;
}

static irqreturn_t ppifcd_irq(int irq, void *dev_id)
{
	size_t count = 0;
	struct uCam_buffer *tmp_buf;
	struct ppi_device_t *pdev = (struct ppi_device_t *)dev_id;
	struct uCam_device_t *uCam_dev = pdev->uCam_dev;
	BUG_ON(dev_id == NULL);

	/*  Acknowledge DMA Interrupt  */
	clear_dma_irqstat(CH_PPI);

	/*  disable ppi  */
	bfin_write_PPI_CONTROL(pdev->ppi_control & ~PORT_EN);

	pdev->done = 1;
	pr_debug("->ppifcd_irq: pdev->done=%d (%ld)\n", pdev->done,
		 jiffies * 1000 / HZ);

	uCam_dev->dma_buf->state = FRAME_DONE;
	pr_debug("->ppifcd_irq: active buffer [0x%p] done\n",
		 uCam_dev->dma_buf->data);
	pr_debug("->ppifcd_irq: next [0x%p] state %d\n",
		 uCam_dev->next_buf->data, uCam_dev->next_buf->state);

	if (waitqueue_active(&uCam_dev->dma_buf->wq))
		wake_up_interruptible(&uCam_dev->dma_buf->wq);

	/* if next frame is ready for grabbing */
	if (uCam_dev->next_buf->state == FRAME_READY) {
		pr_debug("->ppifcd_irq: initiating next grab [0x%p]\n",
			 uCam_dev->next_buf->data);
		tmp_buf = uCam_dev->dma_buf;
		uCam_dev->dma_buf = uCam_dev->next_buf;
		uCam_dev->next_buf = tmp_buf;
		uCam_dev->dma_buf->state = FRAME_GRABBING;
		count =
		    ppi2dma(uCam_dev->ppidev, uCam_dev->dma_buf->data,
			    uCam_dev->size);
	}

	if (pdev->fasyc)
		kill_fasync(&(pdev->fasyc), SIGIO, POLLIN);
	wake_up_interruptible(pdev->rx_avail);

	return IRQ_HANDLED;
}

#if defined(USE_PPI_ERROR)
static irqreturn_t ppifcd_irq_error(int irq, void *dev_id)
{
	BUG_ON(dev_id == NULL);

	printk("-->ppifcd_irq_error: PPI Status = 0x%X\n",
		 bfin_read_PPI_STATUS());
	bfin_clear_PPI_STATUS();

	return IRQ_HANDLED;
}
#endif

static int ppi_fasync(int fd, struct file *filp, int on)
{
	struct ppi_device_t *pdev = uCam_dev->ppidev;
	return fasync_helper(fd, filp, on, &(pdev->fasyc));
}


static int mt9m001_init_v4l(struct mt9m001_data *data)
{
	int err, i;

	pr_debug("Registering uCam device\n");

	err = -ENOMEM;
	uCam_dev = kmalloc(sizeof(struct uCam_device_t), GFP_KERNEL);
	if (uCam_dev == NULL)
		goto error_out;
	uCam_dev->ppidev = kmalloc(sizeof(struct ppi_device_t), GFP_KERNEL);
	if (uCam_dev->ppidev == NULL)
		goto error_out_dev;
	uCam_dev->videodev = kmalloc(sizeof(struct video_device), GFP_KERNEL);
	if (uCam_dev->videodev == NULL)
		goto error_out_ppi;

	pr_debug("  Configuring PPIFCD\n");
	ucam_reg_reset(uCam_dev->ppidev);
	uCam_dev->ppidev->opened = 0;
	uCam_dev->ppidev->uCam_dev = uCam_dev;

	pr_debug("  Configuring Video4Linux driver\n");
	for (i = 0; i < uCAM_NUM_BUFS; i++)
		init_waitqueue_head(&uCam_dev->buffer[i].wq);

	memcpy(uCam_dev->videodev, &uCam_template, sizeof(uCam_template));
	uCam_dev->frame_count = 0;
	err = video_register_device(uCam_dev->videodev, VFL_TYPE_GRABBER, 0);
	if (err) {
		printk(KERN_NOTICE
		       "Unable to register Video4Linux driver for %s\n",
		       uCam_dev->videodev->name);
		goto error_out_video;
	}

	uCam_dev->client = &data->client;
	uCam_dev->lock = SPIN_LOCK_UNLOCKED;
	data->uCam_dev = uCam_dev;

	printk(KERN_INFO "%s: V4L driver %s now ready\n", sensor_name,
	       uCam_dev->videodev->name);

	return 0;

      error_out_video:
	kfree(uCam_dev->videodev);
      error_out_ppi:
	kfree(uCam_dev->ppidev);
      error_out_dev:
	kfree(uCam_dev);
      error_out:
	return err;
}

static int mt9m001_detect_client(struct i2c_adapter *adapter, int address,
				 int kind)
{
	int err;
	struct i2c_client *new_client;
	struct mt9m001_data *data;
	u16 tmp = 0;

	printk(KERN_INFO "%s: detecting client on address 0x%x\n", sensor_name,
	       address << 1);

	if (address != normal_i2c[0] && address != normal_i2c[1])
		return -ENODEV;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return 0;

	data = kzalloc(sizeof(struct mt9m001_data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;
	data->input = 0;
	data->enable = 1;

	i2c_global_client = new_client = &data->client;
	i2c_set_clientdata(new_client, data);
	new_client->addr = address;
	new_client->adapter = adapter;
	new_client->driver = &mt9m001_driver;
	strcpy(new_client->name, sensor_name);

	err = i2c_attach_client(new_client);
	if (err)
		goto error_out;

	pr_debug("%s: detected I2C client (id = %04x)\n", sensor_name, tmp);


	err = cam_control(new_client, CAM_CMD_INIT, 1);

	if (err)
		goto error_out;

	cam_control(new_client, CAM_CMD_SET_PIXFMT, default_palette(force_palette));
	cam_control(new_client, CAM_CMD_SET_FRAMERATE, 30);

	err = mt9m001_init_v4l(data);

	if (err)
		goto error_out;

	return 0;

      error_out:
	kfree(data);
	printk(KERN_ERR "%s: init error 0x%x\n", sensor_name, err);
	return err;
}

static int mt9m001_attach_adapter(struct i2c_adapter *adapter)
{
	int i;
	BUG_ON(adapter == NULL);
	pr_debug("%s: starting probe for adapter %s (0x%x)\n", sensor_name,
		 adapter->name, adapter->id);
	i = i2c_probe(adapter, &addr_data, &mt9m001_detect_client);
	return i;
}

static int mt9m001_detach_client(struct i2c_client *client)
{
	struct mt9m001_data *data;
	int err;

	if ((err = i2c_detach_client(client)))
		return err;

	data = i2c_get_clientdata(client);

	video_unregister_device(data->uCam_dev->videodev);
	kfree(data->uCam_dev->videodev);
	kfree(data->uCam_dev->ppidev);
	kfree(data->uCam_dev);

	kfree(data);

	return 0;
}

static int mt9m001_command(struct i2c_client *client, unsigned int cmd,
			   void *arg)
{
	/* as yet unimplemented */
	return -EINVAL;
}

static struct i2c_driver mt9m001_driver = {
	.driver = {
		   .name = SENSOR_NAME,
		   },
	.id = I2C_DRIVERID_UCAM,
	.attach_adapter = mt9m001_attach_adapter,
	.detach_client = mt9m001_detach_client,
	.command = mt9m001_command,
};

/*
 * FIXME: We should not be putting random things into proc - this is a NO-NO
 */
#if defined(USE_PROC)

static int mt9m001_proc_read(char *buf, char **start, off_t offset, int count,
			     int *eof, void *data)
{
	int i;
	u16 tmp = 0;
	int len = 0;

	for (i = 0; i < sizeof(i2c_regs) / sizeof(*i2c_regs); ++i) {
		uCam_i2c_read(i2c_global_client, i2c_regs[i].regnum, &tmp, 1);
		len += sprintf(&buf[len], "Reg 0x%02x: = 0x%04x %-40s",
			       i2c_regs[i].regnum, tmp, i2c_regs[i].name);
		if (!((i + 1) % 2))
			len += sprintf(&buf[len], "\n");
	}
	if (i % 2)
		len += sprintf(&buf[len], "\n");

	*eof = 1;
	return len;
}

static int mt9m001_proc_write(struct file *file, const char *buf,
			      unsigned long count, void *data)
{
	int reg = 0, val = 0, i;
	sscanf(buf, "%i %i", &reg, &val);
	for (i = 0; i < sizeof(i2c_regs) / sizeof(*i2c_regs); ++i) {
		if (i2c_regs[i].regnum == reg) {
			printk("writing register 0x%02x (%s) with 0x%04x\n",
			       reg, i2c_regs[i].name, val);
			uCam_i2c_write(i2c_global_client, reg, val);
			break;
		}
	}
	return count;
}

static int mt9m001_proc_init(void)
{
	struct proc_dir_entry *ptr;
	pr_debug("  Configuring proc\n");
	ptr = create_proc_entry("uCam", S_IFREG | S_IRUGO, NULL);
	if (!ptr)
		return -1;
	ptr->owner = THIS_MODULE;
	ptr->read_proc = mt9m001_proc_read;
	ptr->write_proc = mt9m001_proc_write;
	return 0;
}
#endif				/* DEBUG PROC FILE */

#if 0
static int v4l2_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		      void *arg)
{
	switch (cmd) {
		/*  query device capabilities
		 * (equiv. to VIDIOCGCAP
		 */
	case VIDIOC_QUERYCAP:{
			struct v4l2_capability *cap = arg;
			pr_debug("VIDIOC_QUERYCAP ioctl called\n");
			memset(cap, 0, sizeof(struct video_capability));
			cap->capabilities =
			    V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE;
			strcpy(cap->driver, "uCam");

			/* driver[16] - canonical name for this device */
			strcpy(cap->card, "Blackfin Cam");
			/* card[32] - canonical name for this device   */

			pr_debug
			    ("  setting 'device capabilities flags' to 0x%08x\n",
			     cap->capabilities);
			return 0;
		}

	case VIDIOC_RESERVED:
		pr_debug("VIDIOC_RESERVED called not implemented\n");
		return 0;

	case VIDIOC_ENUM_FMT:
		pr_debug("VIDIOC_ENUM_FMT called\n");
		return 0;

	case VIDIOC_G_FMT:
		pr_debug("VIDIOC_G_FMT called / not implemented\n");
		return 0;

	case VIDIOC_S_FMT:{
			struct v4l2_format *fmt = arg;
			fmt->fmt.pix.pixelformat = PIXELFORMAT;
			pr_debug("VIDIOC_S_FMT ioctl\n");
			pr_debug("  ...setting 'buffer type' to %d\n",
				 fmt->type);
			pr_debug("  ...setting 'width'  to %d\n",
				 fmt->fmt.pix.width);
			pr_debug("  ...setting 'height' to %d\n",
				 fmt->fmt.pix.height);
			return 0;
		}

	case VIDIOC_G_MPEGCOMP:
		pr_debug("VIDIOC_S_MPEGCOMPcalled / not implemented\n");
		return 0;

	case VIDIOC_S_MPEGCOMP:
		pr_debug("VIDIOC_S_MPEGCOMP called / not implemented\n");
		return 0;

	case VIDIOC_REQBUFS:
		pr_debug("VIDIOC_REQBUFS called not implemented\n");
		return 0;

	case VIDIOC_QUERYBUF:
		pr_debug("VIDIOC_QUERYBUF called not implemented\n");
		return 0;

	case VIDIOC_G_FBUF:
		pr_debug("VIDIOC_G_FBUF called not implemented\n");
		return 0;

	case VIDIOC_S_FBUF:
		pr_debug("VIDIOC_S_FBUF called not implemented\n");
		return 0;

	case VIDIOC_OVERLAY:
		pr_debug("VIDIOC_OVERLAY called not implemented\n");
		return 0;

	case VIDIOC_QBUF:
		pr_debug("VIDIOC_QBUF called not implemented\n");
		return 0;

	case VIDIOC_DQBUF:
		pr_debug("VIDIOC_DQBUF called not implemented\n");
		return 0;

	case VIDIOC_STREAMON:
		pr_debug("VIDIOC_STREAMON called not implemented\n");
		return 0;

	case VIDIOC_STREAMOFF:
		pr_debug("VIDIOC_STREAMOFF called not implemented\n");
		return 0;

	case VIDIOC_G_PARM:
		pr_debug("VIDIOC_G_PARM called not implemented\n");
		return 0;

	case VIDIOC_S_PARM:
		pr_debug("VIDIOC_S_PARM called not implemented\n");
		return 0;

	case VIDIOC_G_STD:
		pr_debug("VIDIOC_G_STD called not implemented\n");
		return 0;

	case VIDIOC_S_STD:
		pr_debug("VIDIOC_S_STD called not implemented\n");
		return 0;

	case VIDIOC_ENUMSTD:
		pr_debug
		    ("VIDIOC_ENUMSTD called / not implemented (cf. bfin_v4l2_driver.c)\n");
		return 0;

	case VIDIOC_ENUMINPUT:
		pr_debug("VIDIOC_ENUMINPUT called\n");
		return 0;

	case VIDIOC_G_CTRL:
		pr_debug("VIDIOC_G_CTRL called not implemented\n");
		return 0;

	case VIDIOC_S_CTRL:
		pr_debug("VIDIOC_S_CTRL called not implemented\n");
		return 0;

	case VIDIOC_G_TUNER:
		pr_debug("VIDIOC_G_TUNER called not implemented\n");
		return 0;

	case VIDIOC_S_TUNER:
		pr_debug("VIDIOC_S_TUNER called not implemented\n");
		return 0;

	case VIDIOC_G_AUDIO:
		pr_debug("VIDIOC_G_AUDIO called not implemented\n");
		return 0;

	case VIDIOC_S_AUDIO:
		pr_debug("VIDIOC_S_AUDIO called not implemented\n");
		return 0;

	case VIDIOC_QUERYCTRL:
		pr_debug("VIDIOC_QUERYCTRL called not implemented\n");
		return 0;

	case VIDIOC_QUERYMENU:
		pr_debug("VIDIOC_QUERYMENU called not implemented\n");
		return 0;

	case VIDIOC_G_INPUT:
		pr_debug("VIDIOC_G_INPUT called\n");
		return 0;

	case VIDIOC_S_INPUT:
		pr_debug("VIDIOC_S_INPUT called\n");
		return 0;

	case VIDIOC_G_OUTPUT:{
			int *output = arg;
			*output = 0;
			pr_debug("VIDIOC_G_OUTPUT called\n");
			return 0;
		}

	case VIDIOC_S_OUTPUT:{
			int *output = arg;
			pr_debug("VIDIOC_S_OUTPUT called\n");
			if (*output != 0)
				return -EINVAL;
			return 0;
		}

	case VIDIOC_ENUMOUTPUT:{
			struct v4l2_output *outp = arg;
			pr_debug
			    ("VIDEO_ENUMOUTPUT called / not implemented (cf. bfin_v4l2_driver.c)\n");
			if (outp->index != 0)
				return -EINVAL;
			memset(outp, 0, sizeof(*outp));
			outp->index = 0;
			outp->type = V4L2_OUTPUT_TYPE_ANALOG;
			strncpy(outp->name, "Autodetect", 31);
			return 0;
		}

	case VIDIOC_G_AUDOUT:
		pr_debug("VIDIOC_G_AUDOUT called not implemented\n");
		return 0;

	case VIDIOC_S_AUDOUT:
		pr_debug("VIDIOC_S_AUDOUT called not implemented\n");
		return 0;

	case VIDIOC_G_MODULATOR:
		pr_debug("VIDIOC_G_MODULATOR called not implemented\n");
		return 0;

	case VIDIOC_S_MODULATOR:
		pr_debug("VIDIOC_S_MODULATOR called not implemented\n");
		return 0;

	case VIDIOC_G_FREQUENCY:
		pr_debug("VIDIOC_G_FREQUENCYcalled not implemented\n");
		return 0;

	case VIDIOC_S_FREQUENCY:
		pr_debug("VIDIOC_S_FREQUENCY called not implemented\n");
		return 0;

	case VIDIOC_CROPCAP:
		pr_debug("VIDIOC_CROPCAP called not implemented\n");
		return 0;

	case VIDIOC_G_CROP:
		pr_debug("VIDIOC_G_CROP called not implemented\n");
		return 0;

	case VIDIOC_S_CROP:
		pr_debug("VIDIOC_S_CROP called not implemented\n");
		return 0;

	case VIDIOC_G_JPEGCOMP:
		pr_debug("VIDIOC_G_JPEGCOMP called not implemented\n");
		return 0;

	case VIDIOC_S_JPEGCOMP:
		pr_debug("VIDIOC_S_JPEGCOMP called not implemented\n");
		return 0;

	case VIDIOC_QUERYSTD:
		pr_debug("VIDIOC_QUERYSTD called not implemented\n");
		return 0;

	case VIDIOC_TRY_FMT:
		pr_debug("VIDIOC_TRY_FMT called not implemented\n");
		return 0;

	case VIDIOC_ENUMAUDIO:
		pr_debug("VIDIOC_ENUMAUDIO called not implemented\n");
		return 0;

	case VIDIOC_ENUMAUDOUT:
		pr_debug("VIDIOC_ENUMAUDOUT called not implemented\n");
		return 0;

	case VIDIOC_G_PRIORITY:
		pr_debug("VIDIOC_G_PRIORITY called not implemented\n");
		return 0;

	case VIDIOC_S_PRIORITY:
		pr_debug("VIDIOC_S_PRIORITY called not implemented\n");
		return 0;

	default:
		pr_debug("unknown/unsupported ioctl command (%08x)\n", cmd);
		return -ENOIOCTLCMD;
	}
}
#endif

/* Returns number of bits per pixel (regardless of where they are located;
 * planar or not), or zero for unsupported format.
 */
static inline int
get_depth(int palette)
{
	switch (palette) {
	case VIDEO_PALETTE_GREY:    return 8;
	case VIDEO_PALETTE_YUV420:  return 12;
	case VIDEO_PALETTE_YUV420P: return 12; /* Planar */
	case VIDEO_PALETTE_YUV422:  return 16;
	case VIDEO_PALETTE_RGB565:  return 16;
	case VIDEO_PALETTE_UYVY:    return 16;
	case VIDEO_PALETTE_YUYV:    return 16;
	default:		    return 0;  /* Invalid format */
	}
}


static int v4l_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		     unsigned long arg)
{
	switch (cmd) {
	case VIDIOCGCAP:{
			/* used to obtain the capability information for a video device */
			struct video_capability *cap =
			    (struct video_capability *)arg;
			pr_debug("VIDIOCGCAP ioctl called\n");
			memset(cap, 0, sizeof(struct video_capability));
			cap->type = VID_TYPE_CAPTURE | VID_TYPE_MONOCHROME;

			/*  name[32] - canonical name for this device */
			strcpy(cap->name, "Blackfin CMOS Camera");

			/* channels - number of radio/tv channels if appropriate */
			cap->channels = 1;

			/* audios   - number of audio devices if appropriate */
			cap->audios = 0;

			/*  maxwidth - maximum capture width in pixels */
			cap->maxwidth = MAX_FRAME_WIDTH;
			uCam_dev->width = 0;

			/* maxheight - maximum capture height in pixels */
			cap->maxheight = MAX_FRAME_HEIGHT; 
			uCam_dev->height = 0;

			/* minwidth - minimum capture width in pixels */
			cap->minwidth = MIN_FRAME_WIDTH;

			/* minheight - minimum capture height in pixels */
			cap->minheight = MIN_FRAME_HEIGHT;

			pr_debug("  setting 'type of interface' to 0x%08x\n",
				 cap->type);
			pr_debug("  setting 'name' to %s\n", cap->name);
			pr_debug("  setting 'channels' to %d\n", cap->channels);
			pr_debug("  setting 'audios' to %d\n", cap->audios);
			pr_debug("  setting 'maxwidth' to %d\n", cap->maxwidth);
			pr_debug("  setting 'maxheight' to %d\n",
				 cap->maxheight);
			pr_debug("  setting 'minwidth' to %d\n", cap->minwidth);
			pr_debug("  setting 'minheight' to %d\n",
				 cap->minheight);
			return 0;
		}

	case VIDIOCGCHAN:{
			/* enumerate the video inputs of a V4L device */
			struct video_channel *v = (struct video_channel *)arg;
			pr_debug("VIDIOCGCHAN called\n");
			if (v->channel != 0) {
				return -EINVAL;
			}
			v->flags = 0;
			v->tuners = 0;
			v->type = VIDEO_TYPE_CAMERA;
			strcpy(v->name, "Blackfin Camera");
			return 0;
		}

	case VIDIOCSCHAN:
		pr_debug("VIDIOCSCHAN called\n");
		pr_debug("  ...command needs more full implementation\n");
		return 0;

	case VIDIOCGTUNER:
		pr_debug("VIDIOCGTUNER called\n");
		pr_debug("  device is not a tuner\n");
		return -EINVAL;

	case VIDIOCSTUNER:
		pr_debug("  ...device is not a tuner\n");
		return -EINVAL;

	case VIDIOCGPICT:{
			struct video_picture *p = (struct video_picture *)arg;
			pr_debug("VIDIOCGPICT called\n");
			p->palette = default_palette(force_palette);
			p->depth = get_depth(default_palette(force_palette));
			return 0;
		}

	case VIDIOCSPICT:{
			struct video_picture *p = (struct video_picture *)arg;
			pr_debug("VIDIOCSPICT called\n");
			if (p->depth != get_depth(default_palette(force_palette))) {
				pr_debug("  not a valid depth (%d)\n",
					 p->depth);
				return -EINVAL;
			}
			if (p->palette != default_palette(force_palette)) {
				pr_debug("  not a valid format (%d)\n",
					 p->palette);
				return -EINVAL;
			}
			
			return 0;
		}

	case VIDIOCCAPTURE:{
			pr_debug("VIDIOCCAPTURE called\n");
			pr_debug("  ...not valid command\n");
			return -EINVAL;
		}

	case VIDIOCGWIN:{
			struct video_window *vw = (struct video_window *)arg;
			pr_debug("VIDIOCGWIN called\n");
			memset(vw, 0, sizeof(*vw));
			return 0;
		}

	case VIDIOCSWIN:{
			struct video_window *vw = (struct video_window *)arg;
			pr_debug("VIDIOCSWIN called\n");
			if (vw->flags) {
				pr_debug("  ...no valid flags\n");
				return -EINVAL;
			}
			if (vw->height < MIN_FRAME_HEIGHT
			    || vw->height > MAX_FRAME_HEIGHT) {
				printk("  ...no valid height\n");
				return -EINVAL;
			}
			if (vw->width < MIN_FRAME_WIDTH
			    || vw->width > MAX_FRAME_WIDTH) {
				printk("  ...no valid width \n");
				return -EINVAL;
			}

			printk("  ...using %dx%d window\n", vw->width,
				 vw->height);
			uCam_dev->width = vw->width;
			uCam_dev->height = vw->height;

			return 0;
		}

	case VIDIOCGFBUF:
		pr_debug("VIDIOCGFBUF called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCSFBUF:
		pr_debug("VIDIOCSFBUF called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCKEY:
		pr_debug("VIDIOCKEY called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCGFREQ:
		pr_debug("VIDIOCGFREQ called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCSFREQ:
		pr_debug("VIDIOCSFREQ called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCGAUDIO:
		pr_debug("VIDIOCGAUDIO called\n");
		pr_debug("  device does not support audio\n");
		return -EINVAL;

	case VIDIOCSAUDIO:
		pr_debug("VIDIOCSAUDIO called\n");
		pr_debug("  device does not support audio\n");
		/* return -EINVAL; */
		return 0;

	case VIDIOCGMBUF:{
			/* reports the size of buffer to mmap and
			 * the offset within the buffer for each frame
			 */
			struct video_mbuf *vm = (struct video_mbuf *)arg;

			pr_debug("VIDIOCGMBUF called (%ld)\n",
				 jiffies * 1000 / HZ);
			memset(vm, 0, sizeof(struct video_mbuf));
			uCam_dev->size = MAX_FRAME_HEIGHT * MAX_FRAME_WIDTH * (get_depth(default_palette(force_palette))/8);
			pr_debug("  capture %zi byte, %dx%d (WxH) frame\n",
				 uCam_dev->size,
				 uCam_dev->width, uCam_dev->height);

			vm->frames = uCAM_NUM_BUFS;
			vm->size = uCam_dev->size;
			vm->offsets[0] = 0x00000000;
			vm->offsets[1] = (u32) top_buffer - 0x1000;
			uCam_dev->buffer[0].data = (void *)0x00001000;
			uCam_dev->buffer[1].data = (void *)top_buffer;

			uCam_dev->dma_buf = &uCam_dev->buffer[0];
			uCam_dev->ready_buf = &uCam_dev->buffer[0];
			uCam_dev->next_buf = &uCam_dev->buffer[1];
			return 0;
		}

	case VIDIOCMCAPTURE:{
			/* VIDIOMCAPTURE starts the capture to frame
			 * when it returns, the frame is not captured yet -
			 * the driver just instructed PPI to start capture
			 * The userspace app has to use VIDIOCSYNC to wait
			 * until the capture of a frame is finished
			 */
			int i;

			size_t count;
			struct video_mmap *vm = (struct video_mmap *)arg;

			BUG_ON(vm == NULL);
			BUG_ON(uCam_dev == NULL);
			BUG_ON(uCam_dev->ppidev == NULL);
			BUG_ON(uCam_dev->ppidev->rx_avail == NULL);

			i = vm->frame;

			pr_debug("VIDIOCMCAPTURE(%d) called (%ld)\n", i,
				 jiffies * 1000 / HZ);
			
			if (i >= uCAM_NUM_BUFS) {
				pr_debug("VIDIOCMCAPTURE: invalid frame (%d)",
					 vm->frame);
				return -EINVAL;
			}

			if (uCam_dev->height != vm->height
			    || uCam_dev->width != vm->width) {
				
				uCam_dev->height = vm->height;
				uCam_dev->width = vm->width;

				bfin_write_PPI_CONTROL(uCam_dev->ppidev->
						       ppi_control & ~PORT_EN);

				cam_control(uCam_dev->client, CAM_CMD_SET_RESOLUTION, RES(uCam_dev->width,uCam_dev->height));
				
				uCam_dev->ppidev->pixel_per_line =
				    uCam_dev->width;
				uCam_dev->ppidev->lines_per_frame =
				    uCam_dev->height;

				set_dma_config(CH_PPI,
					       uCam_dev->ppidev->dma_config);


				if (uCam_dev->ppidev->bpp > 8)
					set_dma_x_count(CH_PPI,
						uCam_dev->ppidev->pixel_per_line);
				else
					set_dma_x_count(CH_PPI,
						uCam_dev->ppidev->pixel_per_line / 2);
				/* Div 2 because of 16-bit packing */


				set_dma_y_count(CH_PPI,
						uCam_dev->ppidev->lines_per_frame);

				bfin_write_PPI_FRAME(uCam_dev->ppidev->
						     lines_per_frame);

				bfin_write_PPI_DELAY(uCam_dev->ppidev->
						     ppi_delay);



				if (uCam_dev->ppidev->bpp > 8)
					bfin_write_PPI_COUNT(uCam_dev->ppidev->
							     pixel_per_line * 2 - 1);
				else
					bfin_write_PPI_COUNT(uCam_dev->ppidev->
							     pixel_per_line - 1);



				if (uCam_dev->ppidev->bpp > 8
				    || uCam_dev->ppidev->dma_config & WDSIZE_16) {
					set_dma_x_modify(CH_PPI, 2);
					set_dma_y_modify(CH_PPI, 2);
				
				}else {
					set_dma_x_modify(CH_PPI, 1);
					set_dma_y_modify(CH_PPI, 1);
				}		

				pr_debug("  setting PPI to %dx%d\n",
					 uCam_dev->ppidev->pixel_per_line,
					 uCam_dev->ppidev->lines_per_frame);

				uCam_dev->size = uCam_dev->width * uCam_dev->height * 
					(get_depth(default_palette(force_palette))/8);

			}

			pr_debug("  capture %zi byte, %dx%d (WxH) frame\n",
				 uCam_dev->size,
				 uCam_dev->width, uCam_dev->height);

			uCam_dev->buffer[i].state = FRAME_READY;

			spin_lock(uCam_dev->lock);
			/* if DMA not busy, initiate DMA
			 *  ow DMA handled by interrupt
			 */
			if (uCam_dev->ppidev->done) {
				if (perfnum) {
					uCam_dev->buffer[i].scyc = cycles();
					uCam_dev->buffer[i].stime = jiffies;
				}
				GPIO_SET_VALUE(uCAM_LEDS, 1);
				uCam_dev->dma_buf = &uCam_dev->buffer[i];
				uCam_dev->dma_buf->state = FRAME_GRABBING;
				pr_debug("  grabbing frame %d [0x%p]\n", i,
					 uCam_dev->dma_buf->data);
				count =
				    ppi2dma(uCam_dev->ppidev,
					    uCam_dev->dma_buf->data,
					    uCam_dev->size);
			} else {
				uCam_dev->next_buf = &uCam_dev->buffer[i];
				pr_debug
				    ("  PPI busy with [0x%p] - ISR will capture to [0x%p] later\n",
				     uCam_dev->dma_buf->data,
				     uCam_dev->buffer[i].data);
			}
			spin_unlock(uCam_dev->lock);

			return 0;
		}

	case VIDIOCSYNC:{
			/* VIDIOCSYNC takes the frame number you want as argument
			 * and waits until the capture of that frame is finished
			 */
			unsigned int i = *((unsigned int *)arg);
			int ret;
			pr_debug("VIDIOCSYNC(%d) called (%ld)\n", i,
				 jiffies * 1000 / HZ);

			switch (uCam_dev->buffer[i].state) {
			case FRAME_UNUSED:
				return -EINVAL;
			case FRAME_READY:
			case VIDIOCSYNC:
			      redo:
				if (!uCam_dev->ppidev)
					return -EIO;

				ret =
				    wait_event_interruptible(uCam_dev->
							     buffer[i].wq,
							     (uCam_dev->
							      buffer[i].state ==
							      FRAME_DONE));
				if (ret)
					return -EINTR;
				pr_debug
				    ("Synch Ready on frame %d, grabstate = %d",
				     i, uCam_dev->buffer[i].state);
				if (uCam_dev->buffer[i].state == FRAME_ERROR) {
					goto redo;
				}
			case FRAME_ERROR:
				/* because irq_err does not restart dma fall through */
			case FRAME_DONE:
				uCam_dev->buffer[i].state = FRAME_UNUSED;

				blackfin_dcache_invalidate_range((u_long)
								 uCam_dev->
								 dma_buf->data,
								 (u_long)
								 (uCam_dev->
								  dma_buf->data +
								  uCam_dev->
								  size));

				uCam_dev->ready_buf = &uCam_dev->buffer[i];
				pr_debug("  ready_buf = [0x%p]\n",
					 uCam_dev->ready_buf->data);

				uCam_dev->frame_count++;

				if (perfnum) {
					uCam_dev->buffer[i].ecyc = cycles();
					uCam_dev->buffer[i].etime = jiffies;
					printk
					    ("  frame %d(0x%p): %-8d cycles, %ld msec\n",
					     uCam_dev->frame_count,
					     uCam_dev->ready_buf->data,
					     uCam_dev->buffer[i].ecyc -
					     uCam_dev->buffer[i].scyc,
					     (uCam_dev->buffer[i].etime -
					      uCam_dev->buffer[i].stime) *
					     1000 / HZ);
				}
				break;
			}
			return 0;
		}

	case VIDIOCGUNIT:
		pr_debug("VIDIOCGUNIT called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCGCAPTURE:
		pr_debug("VIDIOCGCAPTURE called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCSCAPTURE:
		pr_debug("VIDIOCSCAPTURE called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCSPLAYMODE:
		pr_debug("VIDIOCSPLAYMODE called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCSWRITEMODE:
		pr_debug("VIDIOCSWRITEMODE called\n");
		pr_debug("  ...not valid command\n");
		return -EINVAL;

	case VIDIOCGPLAYINFO:
		pr_debug("VIDIOCGPLAYINFO called\n");
		return -EINVAL;

	case VIDIOCSMICROCODE:
		pr_debug("VIDIOCSMICROCODE called\n");
		return -EINVAL;

	case VIDIOCGVBIFMT:
		pr_debug("VIDIOCGVBIFMT called\n");
		return -EINVAL;

	case VIDIOCSVBIFMT:
		pr_debug("VIDIOCSVBIFMT called\n");
		return -EINVAL;

	default:
		pr_debug("unknown/unsupported V4L ioctl command (%08x)\n", cmd);
		return -ENOIOCTLCMD;
	}
	return 0;
}



static void v4l_release(struct video_device *vdev)
{
	kfree(vdev);
}

static int uCam_open(struct inode *inode, struct file *filp)
{
	pr_debug("uCam_open called\n");

	try_module_get(THIS_MODULE);

	if (!uCam_dev) {
		printk("  ...specified video device not found!\n");
		return -ENODEV;
	}

	pr_debug("uCam_open:\n");

	/* FIXME: use a proper mutex here */
	if (uCam_dev->ppidev->opened) {
		printk("  ppi opened already (%d users)\n", uCam_dev->user);
		return -EMFILE;
	}

#if !defined(USE_2ND_BUF_IN_CACHED_MEM)
	top_buffer =
	    dma_alloc_coherent(NULL, MAX_BUFFER_SIZE, &dma_handle, GFP_KERNEL);
#else
	top_buffer =kmalloc(MAX_BUFFER_SIZE, GFP_KERNEL);
#endif
	if (NULL == top_buffer) {
		printk(KERN_ERR ": couldn't allocate dma buffer.\n");
		return -ENOMEM;
	}

	memset(uCam_dev->ppidev, 0, sizeof(struct ppi_device_t));

	uCam_dev->ppidev->opened = 1;
	pr_debug("uCam open setting PPI done\n");
	uCam_dev->ppidev->done = 1;	/* initially ppi is "done" */
	uCam_dev->ppidev->dma_config =
	    (DMA_FLOW_MODE | WNR | RESTART | DMA_WDSIZE_16 | DMA2D | DI_EN);
	uCam_dev->ppidev->pixel_per_line = PIXEL_PER_LINE;
	uCam_dev->ppidev->lines_per_frame = LINES_PER_FRAME;
	uCam_dev->ppidev->bpp = DEFAULT_DEPTH;
	uCam_dev->ppidev->ppi_control =
	    POL_S | POL_C | PPI_DATA_LEN | PPI_PACKING | CFG_GP_Input_3Syncs |
	    GP_Input_Mode;
	uCam_dev->ppidev->ppi_status = 0;
	uCam_dev->ppidev->ppi_delay = 0;
	uCam_dev->ppidev->ppi_trigger_gpio = NO_TRIGGER;
	uCam_dev->ppidev->rx_avail = &uCam_waitqueue;
	uCam_dev->ppidev->irqnum = IRQ_PPI;
	uCam_dev->ppidev->nonblock = 1;
	uCam_dev->ppidev->uCam_dev = uCam_dev;

	if (request_dma(CH_PPI, "PPI_DMA") < 0) {
		printk(KERN_ERR "%s: Unable to attach PPI DMA channel\n",
		       sensor_name);
		return -EFAULT;
	} else
		set_dma_callback(CH_PPI, ppifcd_irq, uCam_dev->ppidev);

#if defined(USE_PPI_ERROR)
	if (request_irq
	    (IRQ_PPI_ERROR, ppifcd_irq_error, 0, "PPI ERROR",
	     uCam_dev->ppidev)) {
		printk(KERN_ERR "%s: Unable to attach PPI error IRQ\n",
		       sensor_name);
		free_dma(CH_PPI);
		return -EFAULT;
	}
#endif

#if (defined(CONFIG_BF537) || defined(CONFIG_BF536) || defined(CONFIG_BF534))
	bfin_write_PORTG_FER(0x00FF);
	bfin_write_PORTF_FER(bfin_read_PORTF_FER() | 0x8300);
	bfin_write_PORT_MUX(bfin_read_PORT_MUX() & ~0x0E00);
#endif

	pr_debug("  specified video device opened sucessfullly\n");
	uCam_dev->user++;

	return 0;
}

static ssize_t uCam_read(struct file *filp, char *buf, size_t count,
			 loff_t * pos)
{
	int Hoff = 12;
	int Woff = 20;
	ssize_t res = 0;

	pr_debug("uCam_read called\n");

	GPIO_SET_VALUE(uCAM_LEDS, 1);
	GPIO_SET_VALUE(uCAM_TRIGGER, 1);

	/* Window control registers
	 * 0x01 10:0 first row to be read out (default 0x000C, 12)
	 * 0x02 10:0 first col to be read out (default 0x0014, 20)
	 * 0x03 10:0 window height (num rows-1) (default 0x03FF, 1023)
	 * 0x04 10:0 window width  (num cols-1) (default 0x04FF, 1279)
	 * set start X,Y & W,H in Camera via I2C
	 */
	if (uCam_dev->height == 512 && uCam_dev->width == 640) {
		Hoff += 320;
		Woff += 256;
	}

	BUG_ON(uCam_dev->ppidev == NULL);
	uCam_dev->ppidev->pixel_per_line = uCam_dev->width;
	uCam_dev->ppidev->lines_per_frame = uCam_dev->height - 1;
	ucam_reg_reset(uCam_dev->ppidev);
	SSYNC();

	pr_debug
	    ("Frame %d reading %zi bytes %dx%d starting at (%d,%d) from pos (start at 0x%p) ...  ",
	     uCam_dev->frame_count, count, uCam_dev->width, uCam_dev->height,
	     Hoff, Woff, pos);
	pr_debug("ppi_count and ppi_frame are %d,%d\n", bfin_read_PPI_COUNT(),
		 bfin_read_PPI_FRAME());

	res = ppi2dma(uCam_dev->ppidev, uCam_dev->buffer[0].data, count);

	pr_debug("done (read %zi/%zi bytes)\n", res, count);
	uCam_dev->frame_count++;

	GPIO_SET_VALUE(uCAM_LEDS, 0);
	GPIO_SET_VALUE(uCAM_TRIGGER, 0);

	return res;
}

static int uCam_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		      unsigned long arg)
{
	return video_usercopy(inode, filp, cmd, arg, (void *)v4l_ioctl);
}

static int uCam_mmap(struct file *filp, struct vm_area_struct *vma)
{
	BUG_ON(uCam_dev == NULL);
	vma->vm_flags |= VM_MAYSHARE;
	vma->vm_start = (u32) uCam_dev->ready_buf->data;
	vma->vm_end = vma->vm_start + (MAX_FRAME_HEIGHT * MAX_FRAME_WIDTH * (get_depth(default_palette(force_palette))/8));

	pr_debug("uCam_mmap: vm mapped to [0x%p-0x%p]\n", (void *)vma->vm_start,
		 (void *)vma->vm_end);
	return 0;
}

static int uCam_close(struct inode *inode, struct file *filp)
{
	struct ppi_device_t *pdev = uCam_dev->ppidev;
	pr_debug("uCam_close called\n");
#if defined(USE_PPI_ERROR)
	free_irq(IRQ_PPI_ERROR, uCam_dev->ppidev);
#endif

#if !defined(USE_2ND_BUF_IN_CACHED_MEM)
	dma_free_coherent(NULL, MAX_BUFFER_SIZE, top_buffer, dma_handle);
#else
	kfree(top_buffer);
#endif
	ucam_reg_reset(pdev);
	ppi_fasync(-1, filp, 0);
	free_dma(CH_PPI);
	pdev->opened = 0;
	GPIO_SET_VALUE(uCAM_LEDS, 0);
	pr_debug("  ...specified video device closed sucessfullly\n");
	uCam_dev->user--;
	module_put(THIS_MODULE);
	uCam_dev->frame_count = 0;

	return 0;
}

static struct file_operations uCam_fops = {
	.owner = THIS_MODULE,
	.open = uCam_open,
	.release = uCam_close,
	.ioctl = uCam_ioctl,
	.compat_ioctl = (void *)v4l_ioctl,
	.llseek = no_llseek,
	.read = uCam_read,
	.mmap = uCam_mmap,
};

static struct video_device uCam_template = {
	.owner = THIS_MODULE,
	.name = "Blackfin CMOS Camera",
	.type = VID_TYPE_CAPTURE | VID_TYPE_MONOCHROME,
	.type2 = V4L2_CAP_VIDEO_CAPTURE,
	.hardware = VID_HARDWARE_UCAM,
	.fops = &uCam_fops,
	.release = &v4l_release,
	.minor = 0,
};

static __exit void mt9m001_exit(void)
{
	int err;

	cam_control(i2c_global_client, CAM_CMD_EXIT, 1);

	if ((err = i2c_del_driver(&mt9m001_driver))) {
		printk(KERN_WARNING "%s: could not del i2c driver: %i\n",
		       sensor_name, err);
		return;
	}

#if defined(USE_PROC)
	remove_proc_entry("uCam", &proc_root);
#endif
	/*  Turn FS3 frame synch off  */

#if defined(BF533_FAMILY)
	gpio_free(uCAM_FS3);
#endif

#ifdef USE_GPIO
	gpio_free(uCAM_LEDS);
	gpio_free(uCAM_TRIGGER);
	gpio_free(uCAM_STANDBY);
#endif
}

static __init void mt9m001_init_cam_gpios(void)
{
	pr_debug("Initializing camera\n");

#ifdef USE_GPIO

	if (gpio_request(uCAM_LEDS, NULL)) {
		printk(KERN_ERR "uCam_open: Failed ro request GPIO_%d \n",
		       uCAM_LEDS);
		return;
	}
	if (gpio_request(uCAM_TRIGGER, NULL)) {
		printk(KERN_ERR "uCam_open: Failed ro request GPIO_%d \n",
		       uCAM_TRIGGER);
		gpio_free(uCAM_LEDS);
		return;
	}

	if (gpio_request(uCAM_STANDBY, NULL)) {
		printk(KERN_ERR "uCam_open: Failed ro request GPIO_%d \n",
		       uCAM_STANDBY);
		gpio_free(uCAM_LEDS);
		gpio_free(uCAM_TRIGGER);
		return;
	}

	gpio_direction_output(uCAM_LEDS);

	/* this will flash the LEDs to say hello */
	gpio_set_value(uCAM_LEDS, 1);
	mdelay(1);
	gpio_set_value(uCAM_LEDS, 0);

	/* Set trigger mode */
	gpio_direction_output(uCAM_TRIGGER);
	gpio_set_value(uCAM_TRIGGER, 0);

	/* Take out of standby mode */
	gpio_direction_output(uCAM_STANDBY);
	gpio_set_value(uCAM_STANDBY, 0);

#endif

}

static __init int mt9m001_init(void)
{
	int err;
	mt9m001_init_cam_gpios();

	if (global_gain > 127) {
		printk(KERN_WARNING
		       "%s: global gain was above 127; resetting to max of 127\n",
		       sensor_name);
		global_gain = 127;
	}

	err = i2c_add_driver(&mt9m001_driver);
	if (err) {
		printk(KERN_WARNING "%s: could not add i2c driver: %i\n",
		       sensor_name, err);
		return err;
	}

#if defined(USE_PROC)
	if (mt9m001_proc_init())
		printk(KERN_WARNING "Could not create proc entry\n");
#endif
	/*  Turn FS3 frame synch off  */

#if defined(BF533_FAMILY)
	if (gpio_request(uCAM_FS3, NULL)) {
		printk(KERN_ERR "uCam_open: Failed ro request GPIO_%d (FS3)\n",
		       uCAM_FS3);
		return -EBUSY;
	}

	gpio_direction_output(GPIO_3);
	gpio_set_value(uCAM_FS3, 0);
#endif

#if 0
	struct mt9m001_data *data;
	data = kzalloc(sizeof(struct mt9m001_data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;
	data->input = 0;
	data->enable = 1;

	mt9m001_init_v4l(data);
#endif

	printk(KERN_INFO "%s: i2c driver ready\n", sensor_name);
	return 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Benjamin");

module_param(global_gain, int, 0);
module_param(perfnum, int, 0);
module_param(debug, int, 0);
module_param(force_palette, int, 0);

MODULE_PARM_DESC(force_palette,
	"0 = VIDEO_PALETTE_GREY"
	"1 = VIDEO_PALETTE_RGB565"
	"2 = VIDEO_PALETTE_YUV422"
	"3 = VIDEO_PALETTE_UYVY\n");

module_init(mt9m001_init);
module_exit(mt9m001_exit);