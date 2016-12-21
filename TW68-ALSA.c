/*
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Thanks to yiliang for variable audio packet length and more audio
 *  formats support.
 */
#include <linux/module.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#include "TW68.h"
#include "TW68_defines.h"

#define TW68_AUDIO_BEGIN 8

MODULE_DESCRIPTION("alsa driver module for tw68 PCIe capture chip");
MODULE_AUTHOR("Simon Xu");
MODULE_LICENSE("GPL");

#define audio_nCH		8
#define DMA_page		4096
#define MAX_BUFFER		(DMA_page * 4 *audio_nCH)

unsigned int TW68_debug_alsa=0;
module_param(TW68_debug_alsa, int, 0644);
MODULE_PARM_DESC(TW68_debug_alsa,"enable ALSA debug messages");


#define AUDIO_PAGE_SIZE (DMA_page/2)

static long TW68_audio_nPCM = 0;


#if 1
int TW68_dev_set_audio(struct TW68_dev *chip, int samplerate, int bits, int channels, int blksize)
{
	u32 tmp1, tmp2, tmp3;
	tmp1 = tw_read(TW6864_R_AUDIO_CTRL1);
	tmp1 &= 0x0000001F;
	tmp1 |= (125000000/samplerate) << 5;
	tmp1 |= blksize << 19;
	tw_write(TW6864_R_AUDIO_CTRL1,  tmp1);
	
	//tmp2 = ((125000000 / samplerate) << 16) +
	//	((125000000 % samplerate) << 16)/samplerate;
	tmp2 = (((124713160)  <<16)/(samplerate ));
    
	tw_write(TW6864_R_AUDIO_CTRL2,  tmp2);
	tmp3 = tw_read(TW6864_R_AUDIO_CTRL3);
	tmp3 &= ~BIT(8);
	tmp3 |= (bits==8) ? BIT(8) : 0;
	tw_write(TW6864_R_AUDIO_CTRL3, tmp3);
	return 0;
}


void TW68_dev_run_audio(struct TW68_adev *dev, bool b_run)
{
	struct TW68_dev *chip = dev->chip;
	int  ch   = dev->channel_id+TW686X_AUDIO_BEGIN;
	u32  tmp  = tw_read(TW6864_R_DMA_CMD);
	u32  tmp1 = tw_read(TW6864_R_DMA_CHANNEL_ENABLE);
	daprintk(DPRT_LEVEL0, dev, "%s(%d %d)\n", __func__, ch, b_run);
	if (b_run) {
		tmp |= (1<<31);
		tmp |= (1<<ch) | (tmp1 & 0xffff);
		tmp1|= (1<<ch);
	} else {
		if(!(tmp&(1<<ch)) && !(tmp1&(1<<ch))) {
			return;
		}
		tmp |= (tmp1 & 0xffff);
		tmp &= ~(1<<ch);
		tmp1&= ~(1<<ch);
		if( tmp1 == 0 ) {
		    tmp = 0;
		}
	}
	tw_write(TW6864_R_DMA_CHANNEL_ENABLE, tmp1);
	tw_write(TW6864_R_DMA_CMD, tmp);
	(void) tw_read(TW6864_R_DMA_CHANNEL_ENABLE);
	(void) tw_read(TW6864_R_DMA_CMD);

}


int TW68_dev_set_adma_buffer(struct TW68_adev *dev, u32 buf_addr, int pb)
{
    struct TW68_dev *chip = dev->chip;
    if(pb == 0) {
        tw_write(TW6864_R_CH8to15_CONFIG_REG_P(dev->channel_id), buf_addr);
    }
    else {
        tw_write(TW6864_R_CH8to15_CONFIG_REG_B(dev->channel_id), buf_addr);
    }
	//daprintk(DPRT_LEVEL0, dev, "%s(%x %d)\n", __func__, buf_addr, pb);
    return 0;
}
#endif


void TW68_alsa_irq(struct TW68_adev *dev, u32 dma_status, u32 pb_status)
{
	int pb = (pb_status>>(dev->channel_id+TW68_AUDIO_BEGIN)) & 1;

	int len = AUDIO_PAGE_SIZE;
	unsigned char *buf;
	int frames;
	int chunk_length;
	int next_pb;
	struct snd_pcm_runtime *runtime;
	struct snd_pcm_substream *substream;
	int period_elapsed;
	int buffer_pos, period_pos;
	int bytes_per_frame;
	
	if (!atomic_read(&dev->snd_stream))
		return;

	substream = dev->substream;
	runtime = substream->runtime;
	
	buf = (pb == 0) ? dev->dma_area : dev->dma_area + AUDIO_PAGE_SIZE;
	next_pb = (pb == 0) ? 1 : 0;
	
	if (next_pb != dev->pb_flag) {
		dev->chip->stat_audio[dev->channel_id].pb_mismatch++;
		// may want to advance the hw_ptr, but not necessarily.
		// there is really no way to know how many IRQs/frames were missed.
	}
	dev->pb_flag = pb;

	/* do the copy */
	buffer_pos = dev->snd_buffer_pos;
	period_pos = dev->snd_period_pos;
	bytes_per_frame = runtime->frame_bits >> 3;
	frames = bytes_to_frames(runtime, len);
	chunk_length = frames;
	//printk("chunk length %d, len %d\n", frames, len);
	if (buffer_pos + chunk_length >= runtime->buffer_size) {
		size_t cnt = (runtime->buffer_size - buffer_pos) *
			bytes_per_frame;
		memcpy(runtime->dma_area + buffer_pos * bytes_per_frame,
		       buf, cnt);
		memcpy(runtime->dma_area, buf + cnt,
		       chunk_length * bytes_per_frame - cnt);
	} else {
		memcpy(runtime->dma_area + buffer_pos * bytes_per_frame,
		       buf, chunk_length * bytes_per_frame);
	}
	
	buffer_pos += chunk_length;
	period_pos += chunk_length;
	
	if (buffer_pos >= runtime->buffer_size)
		buffer_pos -= runtime->buffer_size;
	
	if (period_pos >= runtime->period_size) {
		period_pos -= runtime->period_size;
		period_elapsed = 1;
	}
	
	snd_pcm_stream_lock(substream);

	dev->snd_buffer_pos = buffer_pos;
	dev->snd_period_pos = period_pos;

	snd_pcm_stream_unlock(substream);

	if (period_elapsed)
		snd_pcm_period_elapsed(substream);
	
	return;

}


static void TW68_snd_trigger_work(struct work_struct *work)
{
	struct TW68_adev *dev = container_of(work, struct TW68_adev, snd_trigger);
	struct snd_pcm_runtime *runtime = dev->substream->runtime;
	unsigned long flags;
	if (atomic_read(&dev->snd_stream)) {
		mod_timer(&dev->chip->delay_resync, jiffies + msecs_to_jiffies(100));
		dev->pb_flag = 1;
		spin_lock_irqsave(&dev->chip->slock, flags);
		TW68_dev_set_audio(dev->chip, runtime->rate, runtime->sample_bits, runtime->channels, AUDIO_PAGE_SIZE);//dev->blksize);
		TW68_dev_run_audio(dev, true);
		spin_unlock_irqrestore(&dev->chip->slock, flags);
	} else {
		spin_lock_irqsave(&dev->chip->slock, flags);
		TW68_dev_run_audio(dev, false);
		spin_unlock_irqrestore(&dev->chip->slock, flags);
	}
}

/*
 * ALSA capture trigger
 *
 *   - One of the ALSA capture callbacks.
 *
 *   Called whenever a capture is started or stopped. Must be defined,
 *   but there's nothing we want to do here
 *
 */
static int snd_card_TW68_capture_trigger(struct snd_pcm_substream * substream,
					  int cmd)
{
	struct TW68_adev *dev = snd_pcm_substream_chip(substream);
	daprintk(DPRT_LEVEL0, dev, "%s(%d)\n", __func__, (cmd == SNDRV_PCM_TRIGGER_START));
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		//case SNDRV_PCM_TRIGGER_RESUME:
		//case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		atomic_set(&dev->snd_stream, 1);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		//case SNDRV_PCM_TRIGGER_SUSPEND:
		//case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		atomic_set(&dev->snd_stream, 0);
		break;
	default:
		return -EINVAL;
	}
	schedule_work(&dev->snd_trigger);
	return 0;
}

/*
 * ALSA PCM preparation
 *
 *   - One of the ALSA capture callbacks.
 *
 *   Called right after the capture device is opened, this function configures
 *  the buffer using the previously defined functions, allocates the memory,
 *  sets up the hardware registers, and then starts the DMA. When this function
 *  returns, the audio should be flowing.
 *
 */

static int snd_card_TW68_capture_prepare(struct snd_pcm_substream * substream)
{
	struct TW68_adev *dev = snd_pcm_substream_chip(substream);
	dev->snd_buffer_pos = 0;
	dev->snd_period_pos = 0;
	return 0;
}

/*
 * ALSA pointer fetching
 *
 *   - One of the ALSA capture callbacks.
 *
 *   Called whenever a period elapses, it must return the current hardware
 *  position of the buffer.
 *   Also resets the read counter used to prevent overruns
 *
 */

static snd_pcm_uframes_t
snd_card_TW68_capture_pointer(struct snd_pcm_substream * substream)
{
    struct TW68_adev *dev = snd_pcm_substream_chip(substream);
    return dev->snd_buffer_pos;
}

/*
 * ALSA hardware capabilities definition
 */

static struct snd_pcm_hardware snd_card_TW68_capture =
{
	.info = SNDRV_PCM_INFO_BLOCK_TRANSFER |
	SNDRV_PCM_INFO_MMAP |
	SNDRV_PCM_INFO_INTERLEAVED |
	SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,// | SNDRV_PCM_FMTBIT_S8,
	.rates = SNDRV_PCM_RATE_8000_48000,//KNOT,
	.rate_min = 8000,
	.rate_max = 48000,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = 1024*1024*2,
	.period_bytes_min = 4096,
	.period_bytes_max = 16384,
	.periods_min      = 2,
	.periods_max      = 64,
};


// ALSA hardware params
static int snd_card_TW68_hw_params(struct snd_pcm_substream * substream,
				      struct snd_pcm_hw_params * hw_params)
{
//	struct TW68_adev *dev = snd_pcm_substream_chip(substream);
	int rv;
	rv = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (rv < 0) {
		printk("S812 could not allocate pages !\n");
		return -ENOMEM;
	}
	return rv;
}

/*
 * ALSA hardware release
 *   - One of the ALSA capture callbacks.
 *   Called after closing the device, but before snd_card_TW68_capture_close
 *   It stops the DMA audio and releases the buffers.
 */
static int snd_card_TW68_hw_free(struct snd_pcm_substream * substream)
{
	snd_pcm_lib_free_pages(substream);
	return 0;
}

// ALSA capture finish
static int snd_card_TW68_capture_close(struct snd_pcm_substream * substream)
{
	struct TW68_adev *dev = snd_pcm_substream_chip(substream);
	daprintk(DPRT_LEVEL0, dev, "%s()\n", __func__);
	//mutex_lock(&dev->chip->start_lock);
        mod_timer(&dev->chip->delay_resync, jiffies+ msecs_to_jiffies(100));
	if (atomic_read(&dev->snd_stream)) {
		atomic_set(&dev->snd_stream, 0);
		schedule_work(&dev->snd_trigger);
		flush_work(&dev->snd_trigger);
	}
	//mutex_unlock(&dev->chip->start_lock);
	return 0;
}

/*
 * ALSA capture start
 *
 *   - One of the ALSA capture callbacks.
 *
 *   Called when opening the device. It creates and populates the PCM
 *  structure
 *
 */

static int snd_card_TW68_capture_open(struct snd_pcm_substream * substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct TW68_adev *dev = snd_pcm_substream_chip(substream);
	//mutex_lock(&dev->chip->start_lock);
	mod_timer(&dev->chip->delay_resync, jiffies+ msecs_to_jiffies(100));
	daprintk(DPRT_LEVEL0, dev, "%s()\n", __func__);
	dev->substream = substream;
	runtime->hw = snd_card_TW68_capture;
	//mutex_unlock(&dev->chip->start_lock);
	return 0;
}



/*
 * ALSA capture callbacks definition
 */

static struct snd_pcm_ops snd_card_TW68_capture_ops = {
	.open =			snd_card_TW68_capture_open,
	.close =		snd_card_TW68_capture_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =	        snd_card_TW68_hw_params,
	.hw_free =		snd_card_TW68_hw_free,
	.prepare =		snd_card_TW68_capture_prepare,
	.trigger =		snd_card_TW68_capture_trigger,
	.pointer =		snd_card_TW68_capture_pointer,
};

/*
 * ALSA initialization
 *
 *   Called by the init routine, once for each TW68 device present,
 *  it creates the basic structures and registers the ALSA devices
 *
 */
int TW68_alsa_create(struct TW68_adev *dev)
{

	struct snd_card *card = NULL;
	struct snd_pcm *pcm;
	int rv;
	unsigned long flags = 0;
	daprintk(DPRT_LEVEL0, dev, "%s()\n", __func__);

	if(TW68_audio_nPCM > (SNDRV_CARDS-2)) {
		printk("s812: too many card?\n");
		return -ENODEV;
	}

	INIT_WORK(&dev->snd_trigger, TW68_snd_trigger_work);
	atomic_set(&dev->snd_stream, 0);
	dev->dma_area = pci_alloc_consistent(dev->chip->pci, PAGE_SIZE * 2 * 40, &dev->dma_addr);
	if (dev->dma_area == NULL) {
		printk("dma area fail");
		return -ENOMEM;
	}
	
	spin_lock_irqsave(&dev->chip->slock, flags);
	TW68_dev_set_adma_buffer (dev, dev->dma_addr, 0);
	TW68_dev_set_adma_buffer (dev, dev->dma_addr + AUDIO_PAGE_SIZE, 1);
	spin_unlock_irqrestore(&dev->chip->slock, flags);
	
#if(LINUX_VERSION_CODE > KERNEL_VERSION(2,6,30))
#if(LINUX_VERSION_CODE < KERNEL_VERSION(3,15,0))
	rv = snd_card_create(-1, NULL, THIS_MODULE,
			      0, &card);
	
#else
	rv = snd_card_new(&dev->chip->pci->dev, -1, NULL, THIS_MODULE,
			   0, &card);
#endif
	if (rv < 0) {
		printk("audio card create failed\n");
		return rv;
	}
#else
	card = snd_card_new(-2, NULL, THIS_MODULE, 0);
	if (card == NULL)
		return -ENOMEM;
#endif

	strcpy(card->driver, "TW6869");
	dev->snd = card;
	mutex_init(&dev->lock);
	// previously in separate function, but belongs here
	rv = snd_pcm_new(card, "TW6869 PCM", 0, 0, 1, &pcm);
	if (rv < 0) {
		printk("%s failed snd_pcm_new: %x\n", __func__, rv);
		goto __nodev;
	}
	

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_card_TW68_capture_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
	      snd_dma_continuous_data(GFP_KERNEL), TW_AUDIO_BUFFER,
	      TW_AUDIO_BUFFER);

	pcm->private_data = dev;
	pcm->info_flags = 0;
	strcpy(pcm->name, "TW6869 PCM");
	// set dev is setting "struct device *"
	snd_card_set_dev(card, &dev->chip->pci->dev);
	/* End of "creation" */
	strcpy(card->shortname, "TW6869");
	sprintf(card->longname, "%s at 0x%p irq %d",
		dev->chip->name, dev->chip->bmmio, dev->chip->pci->irq);
	daprintk(1, dev, "alsa: %s registered as card %d\n",card->longname,dev->channel_id);
	rv = snd_card_register(card);
	if (rv < 0)
		goto __nodev;

	//TODO: Sensoray (this counter below is probably not needed)
	TW68_audio_nPCM++;
	return 0;
__nodev:
	printk("S812 ALSA create failed!\n");
	dev->snd = NULL;
	snd_card_free(card);
	printk("%s failed %d\n", __func__, rv);
	return rv;
}

int TW68_alsa_free(struct TW68_adev *dev)
{
	flush_work(&dev->snd_trigger);
	if (dev->dma_area != NULL) {
		pci_free_consistent(dev->chip->pci, PAGE_SIZE * 2 * 40,
				    dev->dma_area, dev->dma_addr);
		dev->dma_area = NULL;
		dev->dma_addr = 0;
	}
	if(dev->snd) {
		snd_card_free(dev->snd);
		dev->snd = NULL;
	}
	TW68_audio_nPCM--;
	return 0;
}

