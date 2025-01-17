
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb/composite.h>
#include <linux/usb/audio.h>
#include <linux/usb/ch9.h>
#include <linux/hid.h>
#include <linux/cdev.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/poll.h>
#include <linux/kfifo.h>
#include <linux/delay.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/platform_device.h>

#define CREATE_TRACE_POINTS
#include "trace.h"

#define DRIVER_DESC "Linux USB Ipod Audio Gadget"
#define DRIVER_VERSION "June, 2016"
#define REPORT_LENGTH 1024

#define BUFFER_BYTES_MAX (PAGE_SIZE * 16)
#define PRD_SIZE_MAX PAGE_SIZE
//#define PRD_SIZE_MIN	1024

#define MIN_PERIODS 4

#define NUM_USB_AUDIO_TRANSFERS 4
#define MAX_USB_AUDIO_PACKET_SIZE 180

#define NUM_HID_IN_TRANSFERS 4

#include "ipod.h"

static struct usb_composite_dev *composite_dev;
static struct usb_composite_driver ipod_driver;

static struct
{
	struct platform_device *pdev;
	struct snd_card *card;
	struct snd_pcm *pcm;

	struct snd_pcm_substream *ss;

	size_t dma_bytes;
	unsigned char *dma_area;
	size_t period_size;

	ssize_t hw_ptr;
	void *rbuf;

	spinlock_t play_lock;

	int alt;
	struct usb_ep *in_ep;
	struct usb_request **in_req;

	int cnt;

} ipod_audio_data;

static struct snd_pcm_hardware ipod_audio_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME,
	.rates = SNDRV_PCM_RATE_44100,
	.rate_min = 44100,
	.rate_max = 44100,
	.buffer_bytes_max = BUFFER_BYTES_MAX,
	//.period_bytes_max = PRD_SIZE_MAX,
	//.period_bytes_min = MAX_USB_AUDIO_PACKET_SIZE,
	.period_bytes_min = 180 / 2,
	.period_bytes_max = PRD_SIZE_MAX,
	.periods_min = MIN_PERIODS,
	.periods_max = BUFFER_BYTES_MAX / PRD_SIZE_MAX,
	.channels_min = 2,
	.channels_max = 2,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
};

static int ipod_audio_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	runtime->hw = ipod_audio_hw;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
	{
		spin_lock_init(&ipod_audio_data.play_lock);
		ipod_audio_data.cnt = 0;
	}

	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);

	return 0;
}

static int ipod_audio_pcm_close(struct snd_pcm_substream *substream)
{
	return 0;
}

static int ipod_audio_pcm_hw_params(struct snd_pcm_substream *substream,
									struct snd_pcm_hw_params *hw_params)
{
	int err = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
	{
		err = snd_pcm_lib_malloc_pages(substream,
									   params_buffer_bytes(hw_params));
		if (err >= 0)
		{
			ipod_audio_data.dma_bytes = substream->runtime->dma_bytes;
			ipod_audio_data.dma_area = substream->runtime->dma_area;
			ipod_audio_data.period_size = params_period_bytes(hw_params);
		}
	}
	return err;
}

static int ipod_audio_pcm_hw_free(struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
	{
		ipod_audio_data.dma_bytes = 0;
		ipod_audio_data.dma_area = NULL;
		ipod_audio_data.period_size = 0;
	}

	return snd_pcm_lib_free_pages(substream);
}

static int ipod_audio_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&ipod_audio_data.play_lock, flags);

	/* Reset */
	ipod_audio_data.hw_ptr = 0;

	switch (cmd)
	{
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		ipod_audio_data.ss = substream;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ipod_audio_data.ss = NULL;
		break;
	default:
		err = -EINVAL;
	}

	spin_unlock_irqrestore(&ipod_audio_data.play_lock, flags);

	/* Clear buffer after Play stops */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && !ipod_audio_data.ss)
		memset(ipod_audio_data.rbuf, 0, MAX_USB_AUDIO_PACKET_SIZE * NUM_USB_AUDIO_TRANSFERS);

	return err;
}

static snd_pcm_uframes_t ipod_audio_pcm_hw_pointer(struct snd_pcm_substream *substream)
{
	return bytes_to_frames(substream->runtime, ipod_audio_data.hw_ptr);
}

static int ipod_audio_pcm_null(struct snd_pcm_substream *substream)
{
	return 0;
}

static struct snd_pcm_ops ipod_audio_pcm_ops = {
	.open = ipod_audio_pcm_open,
	.close = ipod_audio_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = ipod_audio_pcm_hw_params,
	.hw_free = ipod_audio_pcm_hw_free,
	.trigger = ipod_audio_pcm_trigger,
	.pointer = ipod_audio_pcm_hw_pointer,
	.prepare = ipod_audio_pcm_null,
};

static void ipod_audio_iso_complete(struct usb_ep *ep, struct usb_request *req)
{
	unsigned pending;
	unsigned long flags;
	unsigned int hw_ptr;
	bool update_alsa = false;
	struct snd_pcm_substream *substream;

	//trace_ipod_req_out_done(req);

	if (req->status)
		return;

	if (ipod_audio_data.dma_area == NULL)
		goto exit;

	substream = ipod_audio_data.ss;

	if (!substream)
		goto exit;

	spin_lock_irqsave(&ipod_audio_data.play_lock, flags);

	if (ipod_audio_data.cnt < 9)
	{
		req->length = 176;
		req->actual = 176;
	}
	else
	{
		req->length = 180;
		req->actual = 180;
	}
	ipod_audio_data.cnt = (ipod_audio_data.cnt + 1) % 10;

	pending = ipod_audio_data.hw_ptr % ipod_audio_data.period_size;
	pending += req->actual;
	if (pending >= ipod_audio_data.period_size)
		update_alsa = true;

	hw_ptr = ipod_audio_data.hw_ptr;
	ipod_audio_data.hw_ptr = (ipod_audio_data.hw_ptr + req->actual) % ipod_audio_data.dma_bytes;

	spin_unlock_irqrestore(&ipod_audio_data.play_lock, flags);

	/* Pack USB load in ALSA ring buffer */
	pending = ipod_audio_data.dma_bytes - hw_ptr;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
	{
		if (unlikely(pending < req->actual))
		{
			//printk("Oops: %d / %d \n", pending, req->actual);
			memcpy(req->buf, ipod_audio_data.dma_area + hw_ptr, pending);
			memcpy(req->buf + pending, ipod_audio_data.dma_area,
				   req->actual - pending);
		}
		else
		{
			memcpy(req->buf, ipod_audio_data.dma_area + hw_ptr, req->actual);
		}
	}

exit:

	//req->length = MAX_USB_AUDIO_PACKET_SIZE;
	//req->actual = req->length;
	if (usb_ep_queue(ipod_audio_data.in_ep, req, GFP_ATOMIC))
		DBG(composite_dev, "Audio req queue %d Error!\n", __LINE__);

	if (update_alsa)
		snd_pcm_period_elapsed(substream);

	return;
}

int ipod_audio_control_bind(struct usb_configuration *conf, struct usb_function *func)
{
	int ret = 0;
	int i;

	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
	usb_interface_id(conf, func);
	usb_interface_id(conf, func);

	ipod_audio_data.in_ep = usb_ep_autoconfig(conf->cdev->gadget, &ipod_audio_stream_1_endpoint_fs);
	ipod_audio_data.alt = 0;

	ipod_audio_data.rbuf = kzalloc(MAX_USB_AUDIO_PACKET_SIZE * NUM_USB_AUDIO_TRANSFERS, GFP_KERNEL);

	ipod_audio_data.in_req = kzalloc(NUM_USB_AUDIO_TRANSFERS * sizeof(struct usb_request *), GFP_KERNEL);
	for (i = 0; i < NUM_USB_AUDIO_TRANSFERS; i++)
	{
		ipod_audio_data.in_req[i] = NULL;
	}

	func->fs_descriptors = ipod_audio_desc_fs;
	func->hs_descriptors = ipod_audio_desc_hs;

	//not consistent with different kernel versions
	//usb_assign_descriptors(func, ipod_audio_desc_fs, ipod_audio_desc_hs, NULL, NULL);

	//AUDIO CARD
	ipod_audio_data.pdev = platform_device_alloc("snd_usb_ipod", -1);
	if (IS_ERR(ipod_audio_data.pdev))
	{
		ret = PTR_ERR(ipod_audio_data.pdev);
		DBG(composite_dev, "Coudn't create platform device: %d", ret);
		return ret;
	}

	ret = platform_device_add(ipod_audio_data.pdev);
	if (ret)
	{
		DBG(composite_dev, "Coudn't add platform device: %d", ret);
		goto pdev_fail;
	}

	ret = snd_card_new(&ipod_audio_data.pdev->dev, -1, "iPod USB", THIS_MODULE, 0, &ipod_audio_data.card);
	if (ret)
	{
		DBG(composite_dev, "Coudn't create audio card: %d", ret);
		goto pdev_fail;
	}

	ret = snd_pcm_new(ipod_audio_data.card, "iPod PCM", 0, 1, 0, &ipod_audio_data.pcm);
	if (ret)
	{
		DBG(composite_dev, "Coudn't create audio device: %d", ret);
		goto snd_fail;
	}

	snd_pcm_set_ops(ipod_audio_data.pcm, SNDRV_PCM_STREAM_PLAYBACK, &ipod_audio_pcm_ops);

	snd_pcm_lib_preallocate_pages_for_all(ipod_audio_data.pcm, SNDRV_DMA_TYPE_CONTINUOUS,
										  snd_dma_continuous_data(GFP_KERNEL), 0, BUFFER_BYTES_MAX);

	ret = snd_card_register(ipod_audio_data.card);
	if (ret)
	{
		DBG(composite_dev, "Coudn't register audio card: %d", ret);
		goto snd_fail;
	}

	return 0;

snd_fail:
	snd_card_free(ipod_audio_data.card);
	ipod_audio_data.card = NULL;
	ipod_audio_data.pcm = NULL;
pdev_fail:
	platform_device_del(ipod_audio_data.pdev);

	ipod_audio_data.pdev = NULL;

	return ret;
}

void ipod_audio_control_unbind(struct usb_configuration *conf, struct usb_function *func)
{

	int i;

	DBG(conf->cdev, " = %s() \n", __FUNCTION__);

	if (ipod_audio_data.card != NULL)
	{
		snd_card_free(ipod_audio_data.card);

		ipod_audio_data.card = NULL;
		ipod_audio_data.pcm = NULL;

		platform_device_del(ipod_audio_data.pdev);

		ipod_audio_data.pdev = NULL;
	}

	for (i = 0; i < NUM_USB_AUDIO_TRANSFERS; i++)
	{
		if (ipod_audio_data.in_req[i] != NULL)
		{
			usb_ep_free_request(ipod_audio_data.in_ep, ipod_audio_data.in_req[i]);
			ipod_audio_data.in_req[i] = NULL;
		}
	}

	usb_ep_disable(ipod_audio_data.in_ep);

	usb_ep_autoconfig_release(ipod_audio_data.in_ep);
	ipod_audio_data.in_ep = NULL;

	kfree(ipod_audio_data.rbuf);
}

int ipod_audio_control_setup(struct usb_function *func, const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = func->config->cdev;
	struct usb_request *req = cdev->req;

	u16 w_index = le16_to_cpu(ctrl->wIndex);
	u16 w_value = le16_to_cpu(ctrl->wValue);
	u16 w_length = le16_to_cpu(ctrl->wLength);
	int status = 0;
	DBG(func->config->cdev, " = %s() \n", __FUNCTION__);
	DBG(cdev,
		"Control req: %02x.%02x v%04x i%04x l%d\n",
		ctrl->bRequestType, ctrl->bRequest,
		w_value, w_index, w_length);

	switch (ctrl->bRequest)
	{
	case UAC_SET_CUR:
		req->zero = 0;
		req->length = w_length;
		status = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (status < 0)
		{
			ERROR(cdev, "usb_ep_queue error on ep0 %d\n", status);
			return status;
		}

		return status;
		break;
	case UAC_GET_CUR:
		req->zero = 0;
		req->length = w_length;
		status = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (status < 0)
			ERROR(cdev, "usb_ep_queue error on ep0 %d\n", status);
		return status;
		break;
	}

	return -EOPNOTSUPP;
}

int ipod_audio_control_set_alt(struct usb_function *func, unsigned interface, unsigned alt)
{
	int ret = 0;
	int i;
	int err;

	DBG(func->config->cdev, " = %s() \n", __FUNCTION__);

	if (interface == 1)
	{
		if (ipod_audio_data.in_ep != NULL)
		{
			if (alt == 0)
			{
				if (ipod_audio_data.alt == 1)
				{
					for (i = 0; i < NUM_USB_AUDIO_TRANSFERS; i++)
					{
						usb_ep_dequeue(ipod_audio_data.in_ep, ipod_audio_data.in_req[i]);
					}
				}
				usb_ep_disable(ipod_audio_data.in_ep);
				ipod_audio_data.alt = 0;
			}
			else
			{
				usb_ep_disable(ipod_audio_data.in_ep);
				ret = config_ep_by_speed(func->config->cdev->gadget, func, ipod_audio_data.in_ep);
				if (ret)
				{
					DBG(func->config->cdev, "config_ep_by_speed FAILED!\n");
					return ret;
				}
				ret = usb_ep_enable(ipod_audio_data.in_ep);
				if (ret < 0)
				{
					DBG(func->config->cdev, "Enable IN endpoint FAILED!\n");
					return ret;
				}

				ipod_audio_data.alt = 1;
				if (ipod_audio_data.alt == 1)
				{
					//unsigned long flags;
					//spin_lock_irqsave(&ipod_audio_data.play_lock, flags);
					for (i = 0; i < NUM_USB_AUDIO_TRANSFERS; i++)
					{
						ipod_audio_data.in_req[i] = usb_ep_alloc_request(ipod_audio_data.in_ep, GFP_ATOMIC);
						ipod_audio_data.in_req[i]->zero = 0;
						ipod_audio_data.in_req[i]->length = MAX_USB_AUDIO_PACKET_SIZE;
						ipod_audio_data.in_req[i]->complete = ipod_audio_iso_complete;
						ipod_audio_data.in_req[i]->buf = ipod_audio_data.rbuf + i * MAX_USB_AUDIO_PACKET_SIZE;
						err = usb_ep_queue(ipod_audio_data.in_ep, ipod_audio_data.in_req[i], GFP_ATOMIC);
						if (err < 0)
						{
							ERROR(func->config->cdev, "usb_ep_queue error on ep0 %d\n", err);
							//spin_unlock_irqrestore(&ipod_audio_data.play_lock, flags);
							return err;
						}
					}
					//spin_unlock_irqrestore(&ipod_audio_data.play_lock, flags);
				}
			}
		}
	}

	return ret;
}

int ipod_audio_control_get_alt(struct usb_function *func, unsigned interface)
{
	DBG(func->config->cdev, " = %s() \n", __FUNCTION__);

	if (interface == 0)
	{
		return 0;
	}
	else if (interface == 1)
	{
		return ipod_audio_data.alt;
	}
	else
	{
		return -EINVAL;
	}
}

void ipod_audio_control_disable(struct usb_function *func)
{
	DBG(func->config->cdev, " = %s() \n", __FUNCTION__);
}

static struct usb_function ipod_audio_control_function = {
	.bind = ipod_audio_control_bind,
	.unbind = ipod_audio_control_unbind,
	.setup = ipod_audio_control_setup,
	.set_alt = ipod_audio_control_set_alt,
	.get_alt = ipod_audio_control_get_alt,
	.disable = ipod_audio_control_disable

};

// ===== HID handling

// // received report
// struct ipod_report_list_item
// {
// 	struct list_head list;
// 	unsigned int len;
// 	void *buf;
// };

// requests for sending
// struct ipod_req_list_item
// {
// 	struct list_head list;
// 	struct usb_request *req;
// };

static struct
{
	struct usb_ep *in_ep;

	// recv report
	//struct list_head read_report_head;
	STRUCT_KFIFO_REC_2(REPORT_LENGTH*4) read_fifo;
	spinlock_t read_lock;

	// send report
	//struct list_head write_req_head;
	STRUCT_KFIFO_REC_2(REPORT_LENGTH*4) write_fifo;
	spinlock_t write_lock;
	struct usb_request *in_req;
	struct completion in_req_completion;
	struct work_struct send_work;

	wait_queue_head_t waitq;
	
	//char device
	dev_t dev_id;
	struct class *class_id;
	struct cdev cdev;
	struct device *device;

} ipod_hid_data;

// Received new report
static void ipod_hid_recv_complete(struct usb_ep *ep, struct usb_request *req)
{
	//struct ipod_report_list_item *item;
	unsigned long flags;
	trace_printk("begin len=%d actual=%d \n", req->length, req->actual);
	
	if(kfifo_avail(&ipod_hid_data.read_fifo) < req->length) {
		pr_err("ipod-gadget: buffer is full");
		return;
	}
	kfifo_in(&ipod_hid_data.read_fifo, req->buf, req->actual);
	trace_printk("received [ %*ph ]\n", req->length, req->buf);
	wake_up_interruptible(&ipod_hid_data.waitq);
}

// Sent a report
static void ipod_hid_send_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct ipod_req_list_item *item;
	unsigned long flags;
	struct completion* done = req->context;

	// if (req->status != 0) {
	// 	pr_err(" %s() in req error %d \n", __FUNCTION__, req->status);
	// }
	trace_printk("sent [ %*ph ]\n", req->length, req->buf);
	trace_printk("sent done %d/%d\n", req->actual, req->length);
	//trace_printk("status=%d complete len=%d actual=%d \n",req->status, req->length, req->actual);


	// item = kzalloc(sizeof(*item), GFP_ATOMIC);
	// item->req = req;

	// spin_lock_irqsave(&ipod_hid_data.write_lock, flags);
	// list_add_tail(&item->list, &ipod_hid_data.write_req_head);
	// spin_unlock_irqrestore(&ipod_hid_data.write_lock, flags);
	complete(done);
	
	//wake_up_interruptible(&ipod_hid_data.waitq);
}


static ssize_t ipod_hid_dev_read(struct file *file, char __user *buffer,
								 size_t count, loff_t *ptr)
{
	int ret;
	int n, copied;
	unsigned long flags;

	trace_printk("count=%d\n", count);
	if (kfifo_is_empty(&ipod_hid_data.read_fifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
	}
	ret = wait_event_interruptible(ipod_hid_data.waitq,
		!kfifo_is_empty(&ipod_hid_data.read_fifo));
		
	if(ret) {
		return ret;
	}

	n = kfifo_peek_len(&ipod_hid_data.read_fifo);
	if(count < n) {
		return -EFAULT;
	}
	ret = kfifo_to_user(&ipod_hid_data.read_fifo, buffer, count, &copied);
	if(ret) {
		return ret;
	}
	if(WARN_ON(copied != n)) {
		return -EFAULT;
	}
	ret = copied;
	trace_printk("after copy_to_user\n");
	
	return ret;
}

static void ipod_hid_send_workfn(struct work_struct *work) {
	int ret;
	int len;
	trace_printk("started\n");
	while((len = kfifo_out(&ipod_hid_data.write_fifo,
		ipod_hid_data.in_req->buf, REPORT_LENGTH)) > 0) {
		trace_printk("len=%d\n", len);
		//msleep(1000);
		
		ipod_hid_data.in_req->status = 0;
		ipod_hid_data.in_req->zero = 0;
		ipod_hid_data.in_req->length = len;
		ipod_hid_data.in_req->context = &ipod_hid_data.in_req_completion;
		ipod_hid_data.in_req->complete = ipod_hid_send_complete;

		reinit_completion(&ipod_hid_data.in_req_completion);

		if (ret = usb_ep_queue(ipod_hid_data.in_ep, ipod_hid_data.in_req, GFP_ATOMIC)) {
			trace_printk("usb_ep_queue error on int endpoint %zd\n", ret);
			return;
		}

		ret = wait_for_completion_interruptible(&ipod_hid_data.in_req_completion);
		if(ret) {
			trace_printk("wait_for_completion error=%d\n", ret);
			usb_ep_dequeue(ipod_hid_data.in_ep, ipod_hid_data.in_req);
		}
	}
	wake_up_interruptible(&ipod_hid_data.waitq);
	trace_printk("done\n");
}

static ssize_t ipod_hid_dev_write(struct file *file, const char __user *buffer, size_t count, loff_t *offp)
{
	int ret;
	int copied;
	unsigned long flags;
	bool res;
	struct ipod_req_list_item *item;

	trace_printk("count=%d\n", count);

	if (kfifo_avail(&ipod_hid_data.write_fifo) < count) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
	}
	ret = wait_event_interruptible(ipod_hid_data.waitq,
		kfifo_avail(&ipod_hid_data.write_fifo) >= count);
	if(ret) {
		return ret;
	}

	// trace_printk("before lock\n");
	// spin_lock_irqsave(&ipod_hid_data.write_lock, flags);
	// item = list_first_entry(&ipod_hid_data.write_req_head, struct ipod_req_list_item, list);
	// list_del(&item->list);
	// spin_unlock_irqrestore(&ipod_hid_data.write_lock, flags);
	// trace_printk("after lock\n");

	// count = min_t(unsigned, count, REPORT_LENGTH);

	// if (copy_from_user(item->req->buf, buffer, count) != 0) {
	// 	printk("copy_from_user error\n");
	// 	return -EINVAL;
	// }
	// trace_printk("after copy_from_user\n");

	ret = kfifo_from_user(&ipod_hid_data.write_fifo, buffer, count, &copied);
	if(ret) {
		return ret;
	}
	if(WARN_ON(copied != count)) {
		kfifo_skip(&ipod_hid_data.write_fifo);
		return -EFAULT;
	}
	ret = count;

	// item->req->status = 0;
	// item->req->zero = 0;
	// item->req->length = count;
	// item->req->complete = ipod_hid_send_complete;

	// if (ret = usb_ep_queue(ipod_hid_data.in_ep, item->req, GFP_ATOMIC)) {
	// 	printk("usb_ep_queue error on int endpoint %zd\n", ret);
	// 	return ret;
	// }
	// trace_printk("after usb_ep_queue\n");
	trace_printk("after write: %d\n", kfifo_len(&ipod_hid_data.write_fifo));

	res = schedule_work(&ipod_hid_data.send_work);
	trace_printk("schedule=%d\n", res);

	return ret;
}

static unsigned int ipod_hid_dev_poll(struct file *file, poll_table *wait)
{
	unsigned int ret = 0;

	poll_wait(file, &ipod_hid_data.waitq, wait);

	//if (!list_empty(&ipod_hid_data.read_report_head))
	if (!kfifo_is_empty(&ipod_hid_data.read_fifo))
		ret |= POLLIN | POLLRDNORM;

	if (!kfifo_is_full(&ipod_hid_data.write_fifo))
		ret |= POLLOUT | POLLWRNORM;

	return ret;
}


static atomic_t hid_dev_opened;

static int ipod_hid_dev_open(struct inode *inode, struct file *fd)
{
	if(atomic_inc_return(&hid_dev_opened) == 1)
	{
		pr_info("ipod device opened \n");
		usb_composite_probe(&ipod_driver);
	}
	return 0;
}

static int ipod_hid_dev_release(struct inode *inode, struct file *fd)
{
	if(atomic_dec_and_test(&hid_dev_opened))
	{
		usb_composite_unregister(&ipod_driver);
		pr_info("ipod device closed \n");
	}
	return 0;
}

static const struct file_operations ipod_hid_dev_ops = {
	.owner = THIS_MODULE,
	.open = ipod_hid_dev_open,
	.release = ipod_hid_dev_release,
	.write = ipod_hid_dev_write,
	.read = ipod_hid_dev_read,
	.poll = ipod_hid_dev_poll,
	//.llseek		= noop_llseek,
};

// ===== HID

int ipod_hid_bind(struct usb_configuration *conf, struct usb_function *func)
{
	int ret = 0;
	int i;
	unsigned long flags;
	//struct ipod_req_list_item *item;

	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
	usb_interface_id(conf, func);
	func->fs_descriptors = ipod_hid_desc_fs_hs;
	func->hs_descriptors = ipod_hid_desc_fs_hs;

	spin_lock_init(&ipod_hid_data.read_lock);
	spin_lock_init(&ipod_hid_data.write_lock);
	init_waitqueue_head(&ipod_hid_data.waitq);
	INIT_KFIFO(ipod_hid_data.read_fifo);
	INIT_KFIFO(ipod_hid_data.write_fifo);
	INIT_WORK(&ipod_hid_data.send_work, ipod_hid_send_workfn);
	init_completion(&ipod_hid_data.in_req_completion);


	//INIT_LIST_HEAD(&ipod_hid_data.read_report_head);
	//INIT_LIST_HEAD(&ipod_hid_data.write_req_head);


	//usb stuff
	ipod_hid_data.in_ep = usb_ep_autoconfig(conf->cdev->gadget, &ipod_hid_endpoint);
	ipod_hid_data.in_req = usb_ep_alloc_request(ipod_hid_data.in_ep, GFP_KERNEL);
	ipod_hid_data.in_req->buf = kzalloc(REPORT_LENGTH, GFP_KERNEL);

	// for (i = 0; i < NUM_HID_IN_TRANSFERS; i++) {
	// 	item = kzalloc(sizeof(*item), GFP_ATOMIC);
	// 	item->req = usb_ep_alloc_request(ipod_hid_data.in_ep, GFP_KERNEL);
	// 	item->req->buf = kmalloc(REPORT_LENGTH, GFP_KERNEL);
	// 	spin_lock_irqsave(&ipod_hid_data.write_lock, flags);
	// 	list_add_tail(&item->list, &ipod_hid_data.write_req_head);
	// 	spin_unlock_irqrestore(&ipod_hid_data.write_lock, flags);
	// }

	return ret;
}

void ipod_hid_unbind(struct usb_configuration *conf, struct usb_function *func)
{
	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
	cancel_work_sync(&ipod_hid_data.send_work);

	usb_ep_disable(ipod_hid_data.in_ep);
	//kfree(ipod_hid_data.in_req->buf);
	//usb_ep_free_request(ipod_hid_data.in_ep, ipod_hid_data.in_req);

	usb_ep_autoconfig_release(ipod_hid_data.in_ep);
	ipod_hid_data.in_ep = NULL;
}

void ipod_hid_disable(struct usb_function *func)
{
	struct ipod_report_list_item *list, *next;

	DBG(func->config->cdev, " = %s() \n", __FUNCTION__);

	usb_ep_disable(ipod_hid_data.in_ep);

	// list_for_each_entry_safe(list, next, &ipod_hid_data.read_report_head, list)
	// {
	// 	list_del(&list->list);
	// 	kfree(list);
	// }
}

int ipod_hid_setup(struct usb_function *func, const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = func->config->cdev;
	struct usb_request *req = cdev->req;

	u16 w_index = le16_to_cpu(ctrl->wIndex);
	u16 w_value = le16_to_cpu(ctrl->wValue);
	u16 w_length = le16_to_cpu(ctrl->wLength);
	int status = 0;
	int length = w_length;

	DBG(func->config->cdev, " = %s() \n", __FUNCTION__);
	DBG(cdev,
		"Control req: %02x.%02x v%04x i%04x l%d\n",
		ctrl->bRequestType, ctrl->bRequest,
		w_value, w_index, w_length);

	switch (ctrl->bRequest)
	{
	case USB_REQ_GET_DESCRIPTOR:
		VDBG(cdev, "get hid descriptor\n");
		memcpy(req->buf, ipod_hid_report, 208);
		goto respond;
		break;
	case HID_REQ_SET_REPORT:
		req->complete = ipod_hid_recv_complete;
		trace_printk("new report\n");
		goto respond;
		break;
	case HID_REQ_SET_IDLE:
		VDBG(cdev, "set idle \n");
		length = 0;
		goto respond;
		break;
	case 0x40:
		VDBG(cdev, "apple vendor 0x40 \n");
		goto respond;
		break;
	default:
		VDBG(cdev, "unknown request! \n");
	}

stall:
	return -EOPNOTSUPP;

respond:
	req->zero = 0;
	req->length = length;
	status = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
	if (status < 0)
		ERROR(cdev, "usb_ep_queue error on ep0 %d\n", status);
	return status;
}

int ipod_hid_set_alt(struct usb_function *func, unsigned interface, unsigned alt)
{
	int ret = 0;

	DBG(func->config->cdev, " = %s() \n", __FUNCTION__);

	if (ipod_hid_data.in_ep != NULL)
	{
		/* restart endpoint */
		usb_ep_disable(ipod_hid_data.in_ep);

		ret = config_ep_by_speed(func->config->cdev->gadget, func, ipod_hid_data.in_ep);
		if (ret)
		{
			DBG(func->config->cdev, "config_ep_by_speed FAILED!\n");
			return ret;
		}
		ret = usb_ep_enable(ipod_hid_data.in_ep);
		if (ret < 0)
		{
			DBG(func->config->cdev, "Enable IN endpoint FAILED!\n");
			return ret;
		}
	}

	return ret;
}

static bool ipod_hid_req_match(struct usb_function *func,const struct usb_ctrlrequest *ctrl,bool config0) {

	
  	switch(ctrl->bRequest) {
    case 0x40:
    	return true;
  	}


  return false;

}

static struct usb_function ipod_hid_function = {
	.bind = ipod_hid_bind,
	.unbind = ipod_hid_unbind,
	.setup = ipod_hid_setup,
	.set_alt = ipod_hid_set_alt,
	.disable = ipod_hid_disable,
	.req_match = ipod_hid_req_match,

};

// ===== CONFIG

static struct usb_function_instance *fi_ms;
static struct usb_function *f_ms;

int ipod_ptp_config_bind(struct usb_configuration *conf)
{
	int err;
	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
	//usb_add_function(conf, &ipod_audio_control_function);
	//usb_add_function(conf, &ipod_hid_function);
	f_ms = usb_get_function(fi_ms);
	if(IS_ERR(f_ms)) {
		return PTR_ERR(f_ms);
	}
	err = usb_add_function(conf, f_ms);
	if(err < 0) {
		usb_put_function(f_ms);
		return err;
	}

	

	return 0;
}

void ipod_ptp_config_unbind(struct usb_configuration *conf)
{
	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
	if (!IS_ERR_OR_NULL(f_ms)) {
		usb_put_function(f_ms);
	}
}
int ipod_ptp_config_setup(struct usb_configuration *conf, const struct usb_ctrlrequest *ctrl)
{
	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
	return 0;
}

static struct usb_configuration ipod_configuration1 = {
	.label = "PTP",
	.bConfigurationValue = 1,
	/* .iConfiguration = DYNAMIC */
	.bmAttributes = USB_CONFIG_ATT_SELFPOWER,
	.MaxPower = 500,
	.unbind = ipod_ptp_config_unbind,
	.setup = ipod_ptp_config_setup,
};


int ipod_config_bind(struct usb_configuration *conf)
{
	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
	usb_add_function(conf, &ipod_audio_control_function);
	usb_add_function(conf, &ipod_hid_function);
	return 0;
}

void ipod_config_unbind(struct usb_configuration *conf)
{
	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
}
int ipod_config_setup(struct usb_configuration *conf, const struct usb_ctrlrequest *ctrl)
{
	DBG(conf->cdev, " = %s() \n", __FUNCTION__);
	return 0;
}



static struct usb_configuration ipod_configuration2 = {
	.label = "iPod interface",
	.bConfigurationValue = 2,
	/* .iConfiguration = DYNAMIC */
	.bmAttributes = USB_CONFIG_ATT_SELFPOWER,
	.MaxPower = 500,
	.unbind = ipod_config_unbind,
	.setup = ipod_config_setup,
};

// ===== MODULE

static int ipod_bind(struct usb_composite_dev *cdev)
{
	int ret = 0;

	DBG(cdev, " = %s() \n", __FUNCTION__);
	composite_dev = cdev;

	fi_ms = usb_get_function_instance("mass_storage");
	if(IS_ERR(fi_ms)) {
		return PTR_ERR(fi_ms);
	}

	// AUDIO

	//usb_add_config(cdev, &ipod_configuration1, ipod_ptp_config_bind);
	usb_add_config(cdev, &ipod_configuration2, ipod_config_bind);
	return ret;
}

static int ipod_unbind(struct usb_composite_dev *cdev)
{
	DBG(cdev, " = %s() \n", __FUNCTION__);

	composite_dev = NULL;
	if (!IS_ERR_OR_NULL(fi_ms))
		usb_put_function_instance(fi_ms);
	return 0;
}

static void ipod_disconnect(struct usb_composite_dev *cdev)
{
	DBG(cdev, " = %s() \n", __FUNCTION__);
}

static void ipod_suspend(struct usb_composite_dev *cdev)
{
	DBG(cdev, " = %s() \n", __FUNCTION__);
}

static void ipod_resume(struct usb_composite_dev *cdev)
{
	DBG(cdev, " = %s() \n", __FUNCTION__);
}

static struct usb_composite_driver ipod_driver = {
	.name = "g_ipod",
	.dev = &device_desc,
	.strings = ipod_strings,
	.max_speed = USB_SPEED_FULL,

	.bind = ipod_bind,
	.unbind = ipod_unbind,
	.disconnect = ipod_disconnect,

	.suspend = ipod_suspend,
	.resume = ipod_resume,

};

//module_usb_composite_driver(ipod_driver);

static int __init ipod_init(void)
{
	int ret;
	atomic_set(&hid_dev_opened, 0);
	// CHAR DEVICE
	ipod_hid_data.class_id = class_create(THIS_MODULE, "iap");
	if (IS_ERR(ipod_hid_data.class_id))
	{
		ipod_hid_data.class_id = NULL;
		ret = PTR_ERR(ipod_hid_data.class_id);
		printk("Coudn't create class: %d \n", ret);
		return ret;
	}

	ret = alloc_chrdev_region(&ipod_hid_data.dev_id, 0, 1, "iap");
	if (ret)
	{
		class_destroy(ipod_hid_data.class_id);
		printk("Coudn't allocate chrdev: %d \n", ret);
		return ret;
	}

	cdev_init(&ipod_hid_data.cdev, &ipod_hid_dev_ops);

	ret = cdev_add(&ipod_hid_data.cdev, ipod_hid_data.dev_id, 1);
	if (ret)
	{
		printk("Coudn't add cdev: %d \n", ret);
		return ret;
	}

	ipod_hid_data.device = device_create(ipod_hid_data.class_id, NULL, ipod_hid_data.dev_id, NULL, "iap%d", MINOR(ipod_hid_data.dev_id));
	if (IS_ERR(ipod_hid_data.device))
	{
		ipod_hid_data.device = NULL;
		ret = PTR_ERR(ipod_hid_data.device);
		printk("Coudn't create device: %d \n", ret);
		return ret;
	}

	printk("IPOD loaded! \n");
	return 0;
}

static void __exit ipod_exit(void)
{
	if (ipod_hid_data.device != NULL)
	{
		device_destroy(ipod_hid_data.class_id, ipod_hid_data.dev_id);
		ipod_hid_data.device = NULL;
		cdev_del(&ipod_hid_data.cdev);
	}

	unregister_chrdev_region(ipod_hid_data.dev_id, 1);
	class_destroy(ipod_hid_data.class_id);

	printk("IPOD unloaded! \n");
}

module_init(ipod_init);
module_exit(ipod_exit);
//module_driver(__usb_composite_driver, usb_composite_probe,usb_composite_unregister)

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Andrew Onyshchuk");
MODULE_LICENSE("GPL");
