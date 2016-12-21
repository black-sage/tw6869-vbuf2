/*
 *
 * device driver for TW6869 based PCIe capture cards
 * driver core for hardware
 *
 */
// VIDEOBUF2 code and improvements Copyright Sensoray Company Inc.

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/sound.h>
#include <linux/proc_fs.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/dma-mapping.h>
#include <linux/pm.h>

#include "TW68.h"
#include "TW68_defines.h"

MODULE_DESCRIPTION("v4l2 driver module for Sensoray 812[vbuf2]. Version 1.1.13");
MODULE_AUTHOR("Simon Xu 2011-2014 @intersil. Updates/fixes by Sensoray 2011-2016");
MODULE_LICENSE("GPL");

#ifdef CONFIG_PROC_FS
static int tw68_read_show(struct seq_file *m, void *v);
static const struct file_operations tw68_procmem_fops;
#endif


/// 0819 vma buffer   modprobe videobuf_vmalloc
/* ------------------------------------------------------------------ */



static unsigned int irq_debug;
module_param(irq_debug, int, 0644);
MODULE_PARM_DESC(irq_debug,"enable debug messages [IRQ handler]");

static unsigned int core_debug = 0;
module_param(core_debug, int, 0644);
MODULE_PARM_DESC(core_debug,"enable debug messages [core]");

static unsigned int gpio_tracking;
module_param(gpio_tracking, int, 0644);
MODULE_PARM_DESC(gpio_tracking,"enable debug messages [gpio]");

static unsigned int alsa = 1;
module_param(alsa, int, 0644);
MODULE_PARM_DESC(alsa,"enable/disable ALSA DMA sound [dmasound]");

static unsigned int latency = UNSET;
module_param(latency, int, 0444);
MODULE_PARM_DESC(latency,"pci latency timer");

int TW68_no_overlay=-1;
///module_param_named(no_overlay, TW6869_no_overlay, int, 0444);
///MODULE_PARM_DESC(no_overlay,"allow override overlay default (0 disables, 1 enables)"
///		" [some VIA/SIS chipsets are known to have problem with overlay]");


static unsigned int vbi_nr[]   = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };
static unsigned int radio_nr[] = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };
static unsigned int tuner[]    = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };
static unsigned int card[]     = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };



module_param_array(vbi_nr,   int, NULL, 0444);
module_param_array(radio_nr, int, NULL, 0444);
module_param_array(tuner,    int, NULL, 0444);
module_param_array(card,     int, NULL, 0444);


MODULE_PARM_DESC(vbi_nr,   "vbi device number");
MODULE_PARM_DESC(radio_nr, "radio device number");
MODULE_PARM_DESC(tuner,    "tuner type");
MODULE_PARM_DESC(card,     "card type");

DEFINE_MUTEX(TW686v_devlist_lock);
EXPORT_SYMBOL(TW686v_devlist_lock);
LIST_HEAD(TW686v_devlist);
EXPORT_SYMBOL(TW686v_devlist);

#define container_of(ptr, type, member)					\
	({  const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
		(type *)( (char *)__mptr - offsetof(type,member) );})


/* ------------------------------------------------------------------ */

static char name_comp1[]   = "Composite1";
static char name_comp2[]   = "Composite2";
static char name_comp3[]   = "Composite3";
static char name_comp4[]   = "Composite4";
//static char name_svideo[]  = "S-Video";

/* ------------------------------------------------------------------ */
/* board config info                                                  */

static DEFINE_MUTEX(TW68_devlist_lock);

struct TW68_board TW68_boards[] = {
	[TW68_BOARD_UNKNOWN] = {
		.name		= "TW6869",
		.audio_clock	= 0,
		.tuner_type	= TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
				.name = name_comp1,
				.vmux = 0,
				.amux = LINE1,
			}},
	},

	[TW68_BOARD_A] = {
		.name		= "TW6869",
		.audio_clock	= 0,
		.tuner_type	= TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
				.name = name_comp1,
				.vmux = 0,
				.amux = LINE1,
			},{
				.name = name_comp2,
				.vmux = 1,
				.amux = LINE2,
			},{
				.name = name_comp3,
				.vmux = 2,
				.amux = LINE3,
			},{
				.name = name_comp4,
				.vmux = 4,
				.amux = LINE4,
			}},
	},

};

const unsigned int TW68_bcount = ARRAY_SIZE(TW68_boards);

/* ------------------------------------------------------------------ */
/* PCI ids + subsystem IDs                                            */

struct pci_device_id TW68_pci_tbl[] = {

	{
		.vendor       = 0x1797,
		.device       = 0x6869,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
	},
	{
		.vendor       = 0x6000,
		.device       = 0x0812,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
	},
	//
	{
		/* --- end of list --- */
	}
};
MODULE_DEVICE_TABLE(pci, TW68_pci_tbl);
/* ------------------------------------------------------------------ */


static LIST_HEAD(mops_list);
static unsigned int TW68_devcount;


static u32 video_framerate[3][6] = {
	{
		0xBFFFFFFF,	  /// 30  FULL
		0xBFFFCFFF,  /// 28
		0x8FFFCFFF,  /// 26
		0xBF3F3F3F,  /// 24 FPS
		0xB3CFCFC3,  /// 22
		0x8F3CF3CF  /// 20 FPS
	},
	{		30, 28, 26, 24, 22, 20},
	{		25, 23, 20, 18, 16, 14}

};


int (*TW68_dmasound_init)(struct TW68_dev *dev);
int (*TW68_dmasound_exit)(struct TW68_dev *dev);

#define dprintk(fmt, arg...)	if (core_debug)				\
		printk(KERN_DEBUG "%s/core: " fmt, dev->name , ## arg)


void tw68v_set_framerate(struct TW68_dev *dev, u32 ch, u32 n)
{
	if (n >= 0  &&  n < 6)
	{
		if(ch >=0 && ch<8)
			reg_writel( DROP_FIELD_REG0+ ch,  video_framerate[n][0]);   // 30 FPS
		printk("%%%%    tw68v_set_framerate: ch Id %d   n:%d  %d FPS \n ", ch, n,  video_framerate[n][1]);
	}
}



/* ------------------------------------------------------------------ */

int TW68_buffer_pages(int size)
{
	size = PAGE_ALIGN(size);
	size += PAGE_SIZE; /* for non-page-aligned buffers */
	size /= 4096;
	return size;
}

/* calc max # of buffers from size (must not exceed the 4MB virtual
 * address space per DMA channel) */
int TW68_buffer_count(unsigned int size, unsigned int count)
{
	unsigned int maxcount;

	maxcount = 1024 / TW68_buffer_pages(size);
	if (count > maxcount)
		count = maxcount;


	/// printk("TW68_buffer_count  size %d   maxcount %d / C %d \n",  size,	maxcount, count );

	return count;
}


/* ------------------------------------------------------------------ */

int videoDMA_pgtable_alloc(struct pci_dev *pci, struct TW68_pgtable *pt)
{
	__le32      *cpu;
	dma_addr_t   dma_addr, phy_addr;

	cpu = pci_alloc_consistent(pci, PAGE_SIZE<<3, &dma_addr);   // 8* 4096 contiguous  //*2

	if (NULL == cpu)
		return -ENOMEM;

	pt->size = PAGE_SIZE<<3;  ///pt;  //2
	pt->cpu = cpu;
	pt->dma = dma_addr;
	phy_addr = dma_addr + (PAGE_SIZE<<2) + (PAGE_SIZE<<1);  //6 pages
	return 0;
}


void TW68_pgtable_free(struct pci_dev *pci, struct TW68_pgtable *pt)
{
	if (NULL == pt->cpu)
		return;
	pci_free_consistent(pci, pt->size, pt->cpu, pt->dma);
	pt->cpu = NULL;
}


/* ------------------------------------------------------------------ */
/*
 * Buffer handling routines
 *
 * These routines are "generic", i.e. are intended to be used by more
 * than one module, e.g. the video and the transport stream modules.
 * To accomplish this generality, callbacks are used whenever some
 * module-specific test or action is required.
 */

#if 0
void TW68_buffer_finish(struct TW68_dev *dev,
			struct TW68_dmaqueue *q,
			unsigned int state)
{

	if (q->dev != dev)	return;
    if (!q->curr)
            return;
	q->curr->vb.state = state;
	do_gettimeofday(&q->curr->vb.ts);

	/// printk("_buffer_finish ::buffer_finish %p -> NULL  k %p \n",q->curr, k);
	wake_up(&q->curr->vb.done);
	q->curr = NULL;

}

void TW68_buffer_next(struct TW68_dev *dev,
		      struct TW68_dmaqueue *q)
{
        struct TW68_buf *buf,*next = NULL;
        unsigned long flags = 0;
        //BUG_ON(NULL != q->curr);
        if (NULL != q->curr) {
                if ((dev->pcount % 50) == 0) {
                        dprintk("%s: buffer already queued: count %d\n", __func__, dev->pcount);
                }
                dev->pcount++;
                return;
        }
        spin_lock_irqsave(&dev->slock_q, flags);
	if (!list_empty(&q->queued)) {
		/* activate next one from  dma queue */
		buf = list_entry(q->queued.next,struct TW68_buf,vb.queue);

		/// **** remove  v4l video buffer from the queue
		list_del(&buf->vb.queue);
		if (!list_empty(&q->queued))
			next = list_entry(q->queued.next, struct TW68_buf,  vb.queue);
		q->curr = buf;

		buf->vb.state = VIDEOBUF_ACTIVE;
		///printk(KERN_INFO " $$$ buffer_activate  buf->vb.state = VIDEOBUF_ACTIVE; \n");
		mod_timer( &q->timeout, jiffies+BUFFER_TIMEOUT);
	} else {
		/* nothing to do -- just stop DMA */
		del_timer(&q->timeout);
	}
    spin_unlock_irqrestore(&dev->slock_q, flags);
}
#endif

////////////////////////////////////////////////////BFDMA_setup(  );/////////////////////////////////////////////////////////////
void BFDMA_setup(struct TW68_dev *dev, int nDMA_channel, int H, int W)  //    Field0   P B    Field1  P B     WidthHightPitch
{
	u32  regDW, dwV, dn;
	reg_writel((BDMA_ADDR_P_0 + nDMA_channel*8), dev->BDbuf[nDMA_channel][0].dma_addr);						//P DMA page table
	reg_writel((BDMA_ADDR_B_0 + nDMA_channel*8), dev->BDbuf[nDMA_channel][1].dma_addr);	
	//	reg_writel((BDMA_WHP_0 + nDMA_channel*8), (W& 0x7FF) | ((W& 0x7FF)<<11) | ((H &0x3FF)<<22));
	// [31:22] total active lines (height)
	// [21:11] total bytes per line (line_width)
	// [10:0] total active bytes per line (active_width)
	reg_writel((BDMA_WHP_0 + nDMA_channel*8), (W& 0x7FF) | ((W& 0x7FF)<<11) | ((H &0x3FF)<<22));	

	reg_writel((BDMA_ADDR_P_F2_0 + nDMA_channel*8), dev->BDbuf[nDMA_channel][2].dma_addr);						//P DMA page table
	reg_writel((BDMA_ADDR_B_F2_0 + nDMA_channel*8), dev->BDbuf[nDMA_channel][3].dma_addr);	
	reg_writel((BDMA_WHP_F2_0 + nDMA_channel*8), (W& 0x7FF) | ((W& 0x7FF)<<11) | ((H &0x3FF)<<22));	


	regDW = reg_readl( PHASE_REF_CONFIG );
	dn = (nDMA_channel<<1) + 0x10;
	dwV = (0x3 << dn);
	regDW |= dwV;
	reg_writel( PHASE_REF_CONFIG, regDW );
	dwV = reg_readl( PHASE_REF_CONFIG );

	//  printk(KERN_INFO "DMA mode setup %s: %d PHASE_REF_CONFIG  dn 0x%lx    0x%lx  0x%lx  H%d W%d  \n",  
	//		dev->name, nDMA_channel, regDW, dwV,  dn, H, W );


}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

void DecoderResize(struct TW68_dev *dev, int nId, int nHeight, int nWidth)
{
	u32 nAddr, nHW, nH, nW, nVal, nReg, regDW;

	if (nId > 8) {
            printk( "DecoderResize() error: nId:%d,Width=%d,Height=%d\n", nId, nWidth, nHeight);
            return;
	}
	/// printk( "DecoderResize() ::::: nId:%d,Width=%d,Height=%d\n", nId, nWidth, nHeight);
		
	// only for internal 4     HDelay VDelay   etc
	nReg = 0xe6;   //  blue back color
	reg_writel(MISC_CONTROL2, nReg);	
	reg_writel(MISC_CONTROL2+0x100, nReg);

	if (dev->vc[nId].PAL50) {	
            //VDelay
		reg_writel(MISC_CONTROL3, 0xc5);	
		reg_writel(MISC_CONTROL3+ 0x100, 0xc5);	
		regDW = 0x18;
		if (nId <4)
		{
			nAddr = VDELAY0 + (nId *0x10);
			reg_writel(nAddr,  regDW);	

		}
		else
		{
			nAddr = VDELAY0 + ((nId-4) *0x10) + 0x100;
			reg_writel(nAddr,  regDW);	
		}

		//HDelay
		regDW = 0x0A;
		regDW = 0x0C;

		if (nId <4)
		{
			nAddr = HDELAY0 + (nId *0x10);
			reg_writel(nAddr,  regDW);	
		}
		else
		{
			nAddr = HDELAY0 + ((nId-4) *0x10) + 0x100;
			reg_writel(nAddr,  regDW);	

		}
	}
	else
	{
		//VDelay
		reg_writel(MISC_CONTROL3, 0x85);	
		reg_writel(MISC_CONTROL3 + 0x100, 0x85);
		regDW = 0x14;
		if (nId <4)
		{
			nAddr = VDELAY0 + (nId *0x10);
			reg_writel(nAddr,  regDW);	

		}
		else
		{
			nAddr = VDELAY0 + ((nId-4) *0x10) + 0x100;
			reg_writel(nAddr,  regDW);	
		}

		//HDelay
		regDW = 0x0D;
		regDW = 0x0E;

		if (nId <4)
		{
			nAddr = HDELAY0 + (nId *0x10);
			reg_writel(nAddr,  regDW);	
		}
		else
		{
			nAddr = HDELAY0 + ((nId-4) *0x10) + 0x100;
			reg_writel(nAddr,  regDW);	
		}
	}


	///nVal = 0;  ///0x0a;
	///reg_writel(HDELAY0+nId, nVal);   // adjust start pixel
	nVal = reg_readl(HDELAY0+nId);

	// reg_writel (HACTIVE_L0+nId, 0xC0);  // 2C0  704


	nHW = nWidth | (nHeight<<16) | (1<<31);
	nH = nW = nHW;
//	printk( "DecoderResize() :: nId:%d,  H:%d  W:%d   HW %X  HDELAY0 %X\n", nId, nHeight, nWidth, nHW, nVal);   /// read default  0x0a;

	//Video Size
	
	reg_writel(VIDEO_SIZE_REG,  nHW);	    //for Rev.A backward compatible
	reg_writel(VIDEO_SIZE_REG0+nId, nHW);   //for Rev.B or later only

	if (((nHeight == 240) || (nHeight == 288)) &&(nWidth >= 700))
		nWidth = 720;

	if (((nHeight == 240) || (nHeight == 288))&&(nWidth > 699))
		nWidth = 720;
	else
		nWidth = (16*nWidth/720) +nWidth;

	//decoder  Scale 
	nW = nWidth & 0x7FF;
	nW = (720*256)/nW;
	nH = nHeight & 0x1FF;

	///if (nHeight >240)   //PAL
	if(dev->vc[nId].PAL50)
	{
		nH = (288*256)/nH;
	}
	else				//NTSC
	{
		nH = (240*256)/nH;
	}

	if(nId >= 4)
	{
		nAddr = (EX_VSCALE1_LO +  ((nId-4)<<1) + (nId-4)) + 0x100; //EX_VSCALE1_LO + 0|3|6|9
		nAddr = VSCALE1_LO + ((nId-4)<<4) + 0x100; //6869 0x200 VSCALE1_LO + 0|0x10|0x20|0x30

	}
	else
		nAddr = VSCALE1_LO + (nId<<4); //VSCALE1_LO + 0|0x10|0x20|0x30


	nVal = nH & 0xFF;  //V

/*
  if(nId >= 4) 
  {
  ///DeviceWrite2864(dev, nAddr, (unsigned char)nVal);
  ///nReg = DeviceRead2864(dev, nAddr);
  }
  else 
  {
  reg_writel(nAddr,  nVal);	    
  nReg = reg_readl(nAddr);	
  }
*/
	reg_writel(nAddr,  nVal);	    
	nReg = reg_readl(nAddr);	

	nAddr++;  //V H
	nVal = (((nH >> 8) & 0xF) << 4) | (( nW >> 8) & 0xF ); 

	reg_writel(nAddr,  nVal);	    
	nReg = reg_readl(nAddr);	



	nAddr++;  //H
	nVal = nW & 0xFF;


	if(nId >= 4) 
	{
		///DeviceWrite2864(dev, nAddr, (unsigned char)nVal);
		///nReg = DeviceRead2864(dev, nAddr);
	}
	else 
	{
		reg_writel(nAddr,  nVal);	    
		nReg = reg_readl(nAddr);	
	}

	reg_writel(nAddr,  nVal);	    
	nReg = reg_readl(nAddr);	

	//// printk( "DecoderResize() ?????????????????:: nId:%d,    nAddr%X  nReg:%X\n", nId, nAddr, nReg);


	nAddr++;  //H
	nVal = nW & 0xFF;

	if(nId >= 4) 
	{
		///DeviceWrite2864(dev, nAddr, (unsigned char)nVal);
		///nReg = DeviceRead2864(dev, nAddr);
	}
	else 
	{
		reg_writel(nAddr,  nVal);	    
		nReg = reg_readl(nAddr);	
	}

	reg_writel(nAddr,  nVal);	    
	nReg = reg_readl(nAddr);	

// H Scaler
	nVal = (nWidth-12 - 4)*(1<<16)/nWidth;
	nVal = (4 & 0x1F) |
		(((nWidth-12) & 0x3FF) << 5) | 
		(nVal <<15);

	reg_writel(SHSCALER_REG0+nId, nVal);	

}



/////--------------------------------DMA start etc--------------------------------------------------

void resync(unsigned long data)
{
	struct TW68_dev *dev = (struct TW68_dev*)data;
	u32  dwRegE, dwRegF, k, m, mask;
	//unsigned long flags;
	unsigned long	now = jiffies;
    unsigned long flags = 0;
    int videoRS_ID = 0;
	mod_timer(&dev->delay_resync, jiffies+ msecs_to_jiffies(50));

	//if (dev->videoDMA_ID == dev->videoCap_ID)
	//	return;


	if ( now - dev->vc[0].errlog < msecs_to_jiffies(50))
	{
		///printk(" resync _reset_ sync time now%lX - errlog %lX \n", now, dev->errlog[0]);
		return;
	}

	m = 0;
	mask = 0;
    spin_lock_irqsave(&dev->slock, flags);
	for ( k=0; k<8; k++)
	{
		mask = ((dev->videoDMA_ID ^ dev->videoCap_ID) & (1 << k));   
		if ((mask))
		{
			m++;
			videoRS_ID |= mask;
			if((m >1)|| dev->videoDMA_ID)
				k = 16; 
		}
	}
    if (!videoRS_ID) {
            spin_unlock_irqrestore(&dev->slock, flags);
            return;
    }


    dev->videoRS_ID |= videoRS_ID;
	if ((dev->videoDMA_ID == 0) && dev->videoRS_ID)
	{
		int dwRegCur =  reg_readl(DMA_CHANNEL_ENABLE);
		dev->videoDMA_ID = dev->videoRS_ID;
		dwRegE = (dev->videoDMA_ID | (dwRegCur & 0xff00));
		reg_writel(DMA_CHANNEL_ENABLE,dwRegE);
		dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
		dwRegF = (1<<31) | dwRegE;
		dwRegF |= 0xff00;
		reg_writel(DMA_CMD, dwRegF);
		dwRegF = reg_readl(DMA_CMD);
		dev->videoRS_ID = 0;
	}
    spin_unlock_irqrestore(&dev->slock, flags);
}


// special treatment for intel MB
u64 GetDelay(struct TW68_dev *dev, int eno)
{
	u64 	delay, k, last, now, pause;
	last =0;
	pause = 40;  // 20  30  50

/*	
	if (eno > 1)
	pause = 30;     

	if (eno > 3)	
	pause = 60;
*/
		
	/// printk("\n $$$$$$$$$$$$$$$$ go through timers:   jiffies:0x%X ", jiffies);
	for (k=0; k<8; k++)
	{
		/// delay = dev->video_dmaq[k].restarter.expires;
		///printk( " 0X%x ", delay);
		// last = (delay > last) ? delay : last;
 	}

	///delay = (last >jiffies) ? last : jiffies;
	///printk( " last 0X%x   jiffies 0X%x ", last, jiffies);
	now = jiffies;
	if (last > now)
		delay = last;
	else
		delay = jiffies;

	///printk(" delay:0x%X  ", delay);
	if ((delay == jiffies)&& ((last + msecs_to_jiffies(pause)) >delay))
		delay = last + msecs_to_jiffies(pause);
	///printk(" delay:0x%X  ", delay);

	delay += msecs_to_jiffies(pause);

	///printk("\n $$$$$$$$$$$$$$$$ search delay :  40 msec %d        last:0X%x    jiffies:0X%x    delay:0X%x   \n", msecs_to_jiffies(40),  last, jiffies, delay );
	return delay;
}


#if 0
void TW68_buffer_timeout(unsigned long data)
{
	struct TW68_dmaqueue *q = (struct TW68_dmaqueue*)data;
	struct TW68_dev *dev = q->dev;

	if (q->curr) {
		dev->vc[q->id].stat.v4l_timeout++;
		//printk(" TW68_buffer_timeout ????????  DMA %d  || 0x%X   timeout on dma queue %p\n", dwRegE, dwRegF, q->curr);
		TW68_buffer_finish(dev, q, VIDEOBUF_ERROR);
	}
	TW68_buffer_next(dev, q);
}
#endif

/* ------------------------------------------------------------------ */

int TW68_set_dmabits(struct TW68_dev *dev,  unsigned int DMA_nCH)
{
	u32 dwRegE, dwRegF, nId, k, run;
    unsigned long flags = 0;
	nId = DMA_nCH;   ////
    
	spin_lock_irqsave(&dev->slock, flags);
	dwRegE = reg_readl(DMA_CHANNEL_ENABLE);

	dev->video_DMA_1st_started += 1;   //++
//dev->vc[DMA_nCH].video_dmaq.FieldPB = 0;

			
	dwRegE |= (1 << nId);  // +1 + 0  Fixed PB

	dev->videoCap_ID |= (1 << nId) ; 
	dev->videoDMA_ID |= (1 << nId) ; 
	reg_writel(DMA_CHANNEL_ENABLE,dwRegE);

	run = 0;
	
	for (k=0; k<8; k++) {
            if (run < dev->vc[k].videoDMA_run)
                    run = dev->vc[k].videoDMA_run;
	}
	dev->vc[nId].videoDMA_run = run+1;
	dwRegF = (1<<31);
	dwRegF |= dwRegE;
	reg_writel(DMA_CMD, dwRegF);
	spin_unlock_irqrestore(&dev->slock, flags);
	return 0;
}

/////////////////////////////////////////////////////////////
// NOTE: this is called with the device spinlock held
// do not acquire spinlock here
int	stop_video_DMA(struct TW68_dev *dev, unsigned int DMA_nCH)
{
	u32 dwRegE, dwRegF, nId;
	u32 audioCap;
	nId = DMA_nCH;  ///2;

	dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
	audioCap = (dwRegE >> 8) & 0xff;
	dwRegF = reg_readl(DMA_CMD);

	dwRegE &= ~(1 << nId);
	reg_writel(DMA_CHANNEL_ENABLE, dwRegE);
	dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
	dev->videoDMA_ID &= ~(1 << nId);
	dev->videoCap_ID &= ~(1 << nId);

	if ((dev->videoCap_ID != 0)) {
		dwRegF = 0x8000ffffUL;
		dwRegF &= ~(1 << nId);
		reg_writel(DMA_CMD, dwRegF);
		reg_writel(DMA_CMD, dwRegF);
		dwRegF = reg_readl(DMA_CMD);
		dwRegF = reg_readl(DMA_CMD);
	}
	dev->vc[nId].videoDMA_run = 0;

	// [Sensoray]: don't stop the audio!
	// TODO(Sensoray's comment): 
	//       There should be no need to track videoCap_ID
	//       separately with proper mutex access of the register
	//       for DMA_CHANNEL_ENABLE
	if ((dev->videoCap_ID == 0) && !audioCap) {
            reg_writel(DMA_CMD, 0);
            reg_writel(DMA_CHANNEL_ENABLE, 0);
            reg_writel(DMA_CHANNEL_ENABLE, 0);
            (void) reg_readl(DMA_CHANNEL_ENABLE);
	    dev->video_DMA_1st_started =0;  // initial value for skipping startup DMA error
	}
	/// printk(KERN_INFO " -------------stop_video_DMA   DMA_CHANNEL_ENABLE 0x%X,  DMA_CMD 0x%X,  DMA_INT_STATUS 0x%08X \n",
	///             dwRegE, dwRegF,  dwRegST      );
	return 0;
}


int	VideoDecoderDetect(struct TW68_dev *dev, unsigned int DMA_nCH)
{
	u32	regDW, dwReg;

	if (DMA_nCH <4)  // VD 1-4
	{
		regDW = reg_readl(DECODER0_STATUS + (DMA_nCH* 0x10));
		dwReg = reg_readl(DECODER0_SDT + (DMA_nCH* 0x10));
	}
	else		// 6869  VD 5-8
	{
		//regDW = (ULONG)DeviceRead2864(this, VIDEO_STATUS_0 + ((nID -4)* 0x10));
		//dwReg = (ULONG)DeviceRead2864(this, exVD0_SDT + ((nID -4)* 0x10));
		regDW = reg_readl(DECODER0_STATUS + ((DMA_nCH -4)* 0x10) + 0x100);
		dwReg = reg_readl(DECODER0_SDT + ((DMA_nCH -4)* 0x10) + 0x100);
		///printk("\n\n Decoder 0x%X VideoStandardDetect DMA_nCH %d  regDW 0x%x  dwReg%d \n", (DECODER0_STATUS + (DMA_nCH* 0x10) + 0x100), regDW, dwReg );
	}

	if (( regDW &1)) //&& (!(dwReg & 0x80)))   ///skip the detection glitch     //detect properly
	{
		// set to PAL 50 for real...
		// VDelay
		printk("50HZ VideoStandardDetect DMA_nCH %d  regDW 0x%x  dwReg%d \n", DMA_nCH, regDW, dwReg );
		return 50;
	}
	else
	{
		printk("60HZ VideoStandardDetect DMA_nCH %d  regDW 0x%x  dwReg%d \n", DMA_nCH, regDW, dwReg );
		return 60;
			
	}
}

/* ------------------------------------------------------------------ */

static irqreturn_t TW68_irq(int irq, void *dev_id)    /// hardware dev id for the ISR
{	
	struct TW68_dev *dev = (struct TW68_dev*) dev_id;
	unsigned long flags, k, handled;
	u32 dwRegST, dwRegER, dwRegPB, dwRegE, dwRegF, dwRegVP, dwErrBit;

	handled = 1;
	spin_lock_irqsave(&dev->slock, flags);
	dwRegST = reg_readl(DMA_INT_STATUS);
	// DMA_INT_ERROR named FIFO_STATUS in TW6869 manual
	dwRegER = reg_readl(DMA_INT_ERROR);
	dwRegPB = reg_readl(DMA_PB_STATUS);
	dwRegVP = reg_readl(VIDEO_PARSER_STATUS);
	spin_unlock_irqrestore(&dev->slock, flags);

	if (dwRegER & 0xff000000) {
		//printk("DWREGERR error for register %x, %x !\n", dwRegER, dwRegST);
	}
	if (!(dwRegST & 0xffff) && dwRegST) {
            int j;
            for (j = 0; j < 8; j++) {
                    dev->vc[j].stat.dma_timeout++;
            }
            //printk("DWREGST dma timeout?: reg %x, %x !\n", dwRegER, dwRegST);
	}
	if (!dwRegST) {
		// not our interrupt
            //printk("reg error %x, %x !\n", dwRegER, dwRegST);
		handled = 0;
		return IRQ_RETVAL(handled);
	} else {
            //printk("TW68 test only\n");
    }
	///////////////////////////////////////////////////////////////////////////

	if ((dwRegER &0xFF000000) && dev->video_DMA_1st_started)
	{
		dev->err_times++;
		if (dev->err_times > 3) {
			dev->err_times = 0;
		dev->video_DMA_1st_started --;
		if (dev->video_DMA_1st_started < 0)
			dev->video_DMA_1st_started = 0;
		}
		//printk("DeviceInterrupt: 1st startup err_times:%d, fs %d ## dma_status (err) =0x%x   dwRegVP (video parser)=0X%x   int_status 0x%x   \n", dev->err_times, dev->video_DMA_1st_started, dwRegER, dwRegVP, dwRegST);//, dwRegE);
		return IRQ_RETVAL(handled);
	}
	else
	{
		if((dwRegER>>16 ) || dwRegVP || (dwRegST >>24))   //stop
		{ //err_exist, such as cpl error, tlp error, time-out
                int eno;
			dwErrBit = 0;
			dwErrBit |= ((dwRegST >>24) & 0xFF);
			dwErrBit |= (((dwRegVP >>8) |dwRegVP )& 0xFF);
			///dwErrBit |= (((dwRegER >>24) |(dwRegER >>16) |(dwRegER ))& 0xFF);
			dwErrBit |= (((dwRegER >>24) |(dwRegER >>16))& 0xFF);
			spin_lock_irqsave(&dev->slock, flags);
            dwRegE = reg_readl(DMA_CHANNEL_ENABLE);

			eno =0;

			for (k = 0; k < 8; k++) {
				if (dwErrBit & (1 << k)) {
					eno++;
					// Disable DMA channel
					dev->vc[k].stat.dma_err++;
					dwRegE &= ~((1<< k));
					if (eno >2)
						dwRegE &= ~((0xFF));
				}
			}

			// stop  all VIDEO error channels
			dev->videoDMA_ID = dwRegE & 0xff;
			reg_writel(DMA_CHANNEL_ENABLE, dwRegE);
			dwRegF = reg_readl(DMA_CHANNEL_ENABLE);
			dwRegF = (1<<31);
			dwRegF |= dwRegE;
			dwRegF |= 0xff00;
			reg_writel(DMA_CMD, dwRegF);
			dwRegF = reg_readl(DMA_CMD);
			spin_unlock_irqrestore(&dev->slock, flags);
			//printk("DeviceInterrupt: errors  ## dma_status  0x%X   (err) =0x%X   dwRegVP (video parser)=0X%x   int_status 0x%x  # dwRegE 0X%x dwRegF 0X%x \n", 
            //dwErrBit, dwRegER, dwRegVP, dwRegST, dwRegE, dwRegF);
			dev->vc[0].errlog = jiffies;
            dev->last_dmaerr = dwErrBit;
            //wake_up(&dev->dma_wq);
		}
		else
		{
			// Normal interrupt:
			// printk("  Normal interrupt:  ++++ ## dma_status 0x%X   FIFO =0x%X   (video parser)=0X%x   int_status 0x%x  PB 0x%x  # dwRegE 0X%x dwRegF 0X%x \n", 
			//	     dwRegST, dwRegER, dwRegVP,  dwRegST, dwRegPB, dwRegE, dwRegF);
#ifdef AUDIO_BUILD
            for (k = 0; k < TW68_NUM_AUDIO; k++) {
                    if (dwRegST & TW6864_R_DMA_INT_STATUS_DMA(k+TW686X_AUDIO_BEGIN)) {
                            struct TW68_adev *adev;
                            adev = dev->aud_dev[k];
                            TW68_alsa_irq(adev, dwRegST, dwRegPB);
				}
			}
#endif
			
			if ((dwRegST & (0xFF)) && (!(dwRegER >>16))) {
				for (k = 0; k < 8; k++) {
					if ((dwRegST & dev->videoDMA_ID) & (1 << k)) {
						/// exclude  inactive dev
						TW68_irq_video_done(dev, k, dwRegPB);
					}
				}
			}
			
			if (dev->videoRS_ID) {
				int curE = reg_readl(DMA_CHANNEL_ENABLE);
                spin_lock_irqsave(&dev->slock, flags);
				dev->videoDMA_ID |= dev->videoRS_ID;
				dev-> videoRS_ID =0;
				dwRegE = dev->videoDMA_ID | (curE & 0xff00); 
				reg_writel(DMA_CHANNEL_ENABLE, dwRegE);
				dwRegF = (1<<31);
				dwRegF |= dwRegE;
				dwRegF |= 0xff00;

				reg_writel(DMA_CMD, dwRegF);
				dwRegF = reg_readl(DMA_CMD);
                spin_unlock_irqrestore(&dev->slock, flags);
			}
		}
	}

	return IRQ_RETVAL(handled);
}


/* ------------------------------------------------------------------ */

/* early init (no i2c, no irq) */

static int TW68_hwinit1(struct TW68_dev *dev)
{
	u32  m_StartIdx, m_EndIdx, m_nVideoFormat, m_dwCHConfig,   dwReg, \
		m_bHorizontalDecimate, m_bVerticalDecimate, m_nDropChannelNum, \
		m_bDropMasterOrSlave, m_bDropField, m_bDropOddOrEven, m_nCurVideoChannelNum;
	u32	 regDW, val1, addr, k, ChannelOffset, pgn;
	// Audio P
//	int audio_ch;
//	u32 dmaP, dmaB;
	spin_lock_init(&dev->slock);

	/////////////////////////////////////////////////////////////////////////

	pci_read_config_dword( dev->pci, PCI_VENDOR_ID, &regDW);
	// printk(KERN_INFO "%s: found with ID: 0x%lx\n", dev->name,  regDW );

	pci_read_config_dword( dev->pci, PCI_COMMAND, &regDW);   // 04 PCI_COMMAND
	printk(KERN_INFO "%s: CFG[0x04] PCI_COMMAND :  0x%x\n", dev->name,  regDW );
	
	regDW |= 7;
	regDW &= 0xfffffbff;
	pci_write_config_dword( dev->pci, PCI_COMMAND, regDW);
	pci_read_config_dword( dev->pci, 0x4, &regDW);
	//  printk(KERN_INFO "%s: CFG[0x04]   0x%lx\n", dev->name,  regDW );

	pci_read_config_dword( dev->pci, 0x3c, &regDW);
	// printk(KERN_INFO "%s: CFG[0x3c]   0x%lx\n", dev->name,  regDW );

	// MSI CAP     disable MSI
	pci_read_config_dword( dev->pci, 0x50, &regDW);
	regDW &= 0xfffeffff;
	pci_write_config_dword( dev->pci, 0x50, regDW);
	pci_read_config_dword( dev->pci, 0x50, &regDW);
	//  printk(KERN_INFO "%s: CFG[0x50]   0x%lx\n", dev->name,  regDW );
	//  MSIX  CAP    disable
	pci_read_config_dword( dev->pci, 0xac, &regDW);
      	regDW &= 0x7fffffff;
	pci_write_config_dword( dev->pci, 0xac, regDW);
	pci_read_config_dword( dev->pci, 0xac, &regDW);
	//  printk(KERN_INFO "%s: CFG[0xac]   0x%lx\n", dev->name,  regDW );

	// PCIe Cap registers
	pci_read_config_dword( dev->pci, 0x70, &regDW);
	// printk(KERN_INFO "%s: CFG[0x70]   0x%lx\n", dev->name,  regDW );
	pci_read_config_dword( dev->pci, 0x74, &regDW);
	// printk(KERN_INFO "%s: CFG[0x74]   0x%lx\n", dev->name,  regDW );

	pci_read_config_dword( dev->pci, 0x78, &regDW);
	regDW &= 0xfffffe1f;
	regDW |= (0x8 <<5);        ///  8 - 128   ||  9 - 256  || A - 512
	pci_write_config_dword( dev->pci, 0x78, regDW);

	pci_read_config_dword( dev->pci, 0x78, &regDW);
	//  printk(KERN_INFO "%s: CFG[0x78]   0x%lx\n", dev->name,  regDW );

	pci_read_config_dword( dev->pci, 0x730, &regDW);
	//  printk(KERN_INFO "%s: CFG[0x730]   0x%lx\n", dev->name,  regDW );

	pci_read_config_dword( dev->pci, 0x734, &regDW);
	//  printk(KERN_INFO "%s: CFG[0x734]   0x%lx\n", dev->name,  regDW );

	pci_read_config_dword( dev->pci, 0x738, &regDW);
	//  printk(KERN_INFO "%s: CFG[0x738]   0x%lx\n", dev->name,  regDW );

 	mdelay(20);
	reg_writel(DMA_CHANNEL_ENABLE, 0);
	mdelay(50);
	reg_writel(DMA_CMD, 0);
	
	reg_readl(DMA_CHANNEL_ENABLE);
	reg_readl(DMA_CMD);

	//Trasmit Posted FC credit Status
	reg_writel(EP_REG_ADDR, 0x730);   //
	regDW = reg_readl( EP_REG_DATA );
	//printk(KERN_INFO "%s: PCI_CFG[Posted 0x730]= 0x%lx\n", dev->name,  regDW );

	//Trasnmit Non-Posted FC credit Status
	reg_writel(EP_REG_ADDR, 0x734);   //
	regDW = reg_readl( EP_REG_DATA );
	//printk(KERN_INFO "%s: PCI_CFG[Non-Posted 0x734]= 0x%lx\n", dev->name,  regDW );

	//CPL FC credit Status
	reg_writel(EP_REG_ADDR, 0x738);   //
	regDW = reg_readl( EP_REG_DATA );
	//printk(KERN_INFO "%s: PCI_CFG[CPL 0x738]= 0x%lx\n", dev->name,  regDW );
	/////////////////////////////////////////////////////////////////////////////////////////////

	regDW = reg_readl( (SYS_SOFT_RST) );
	/// printk(KERN_INFO "HWinit %s: SYS_SOFT_RST  0x%lx    \n",  dev->name, regDW );
	///regDW = tw_readl( SYS_SOFT_RST );
	///printk(KERN_INFO "DMA %s: SYS_SOFT_RST  0x%lx    \n",  dev->name, regDW );

	reg_writel((SYS_SOFT_RST), 0x01);   //??? 01   09
	reg_writel((SYS_SOFT_RST), 0x0F);
	regDW = reg_readl( SYS_SOFT_RST );
	///printk(KERN_INFO " After software reset DMA %s: SYS_SOFT_RST  0x%lx    \n",  dev->name, regDW );

	regDW = reg_readl( PHASE_REF_CONFIG );
	///printk(KERN_INFO "HWinit %s: PHASE_REF_CONFIG  0x%lx    \n",  dev->name, regDW );
	regDW = 0x1518;
	reg_writel( PHASE_REF_CONFIG, regDW&0xFFFF );

	//  Allocate PB DMA pagetable  total 16K  filled with 0xFF
	videoDMA_pgtable_alloc(dev->pci, &dev->m_Page0);

	ChannelOffset = pgn = 128;   ///125;
	pgn = 85;					///   starting for 720 * 240 * 2
	m_nDropChannelNum = 0;
	m_bDropMasterOrSlave = 1;   // master
	m_bDropField = 0;
	m_bDropOddOrEven = 0;

//	m_nVideoFormat = VIDEO_FORMAT_RGB565;
	m_nVideoFormat = VIDEO_FORMAT_YUYV;
	m_bHorizontalDecimate = 0;
	m_bVerticalDecimate = 0;
	for (k = 0; k <MAX_NUM_SG_DMA; k++)
	{
		m_StartIdx = ChannelOffset * k;
		m_EndIdx = m_StartIdx + pgn;
		m_nCurVideoChannelNum = 0;  // real-time video channel  starts 0
		m_nVideoFormat = 0;   ///0; ///VIDEO_FORMAT_UYVY;

		m_dwCHConfig = ( m_StartIdx&0x3FF)			|    // 10 bits
			((m_EndIdx&0x3FF)<<10)			|	 // 10 bits
			((m_nVideoFormat&7)<<20)		|
			((m_bHorizontalDecimate&1)<<23)|
			((m_bVerticalDecimate&1)<<24)	|
			((m_nDropChannelNum&3)<<25)	|
			((m_bDropMasterOrSlave&1)<<27) |    // 1 bit
			((m_bDropField&1)<<28)			|
			((m_bDropOddOrEven&1)<<29)		|
			((m_nCurVideoChannelNum&3)<<30);

		reg_writel( DMA_CH0_CONFIG+ k,m_dwCHConfig);
		dwReg = reg_readl(DMA_CH0_CONFIG+ k);
		/// printk(" ********#### buffer_setup%d::  m_StartIdx 0X%x  0x%X  dwReg: 0x%X  m_dwCHConfig 0x%X  \n", k, m_StartIdx, pgn,  m_dwCHConfig, dwReg );

		reg_writel( VERTICAL_CTRL, 0x24); //0x26 will cause ch0 and ch1 have dma_error.  0x24
		reg_writel( LOOP_CTRL,   0xA5  );     // 0xfd   0xA5     /// 1005
		reg_writel( DROP_FIELD_REG0+ k, 0);  ///m_nDropFiledReg
	}

	regDW = reg_readl( (DMA_PAGE_TABLE0_ADDR) );
	printk(KERN_INFO "DMA %s: DMA_PAGE_TABLE0_ADDR  0x%x    \n",  dev->name, regDW );
	regDW = reg_readl( (DMA_PAGE_TABLE1_ADDR) );
	printk(KERN_INFO "DMA %s: DMA_PAGE_TABLE1_ADDR  0x%x    \n",  dev->name, regDW );

	reg_writel((DMA_PAGE_TABLE0_ADDR), dev->m_Page0.dma);						//P DMA page table
	reg_writel((DMA_PAGE_TABLE1_ADDR), dev->m_Page0.dma + (PAGE_SIZE <<1));		//B DMA page table


	regDW = reg_readl( (DMA_PAGE_TABLE0_ADDR) );
	printk(KERN_INFO "DMA %s: DMA_PAGE_TABLE0_ADDR  0x%x    \n",  dev->name, regDW );
	regDW = reg_readl( (DMA_PAGE_TABLE1_ADDR) );
	printk(KERN_INFO "DMA %s: DMA_PAGE_TABLE1_ADDR  0x%x    \n",  dev->name, regDW );

	/*
	  regDW = tw_readl( (DMA_PAGE_TABLE0_ADDR) );
	  printk(KERN_INFO "DMA %s: tw DMA_PAGE_TABLE0_ADDR  0x%x    \n",  dev->name, regDW );
	  regDW = tw_readl( (DMA_PAGE_TABLE1_ADDR) );
	  printk(KERN_INFO "DMA %s: tw DMA_PAGE_TABLE1_ADDR  0x%x    \n",  dev->name, regDW );
	*/

	reg_writel(AVSRST,0x3F);        // u32
	regDW = reg_readl(AVSRST);
	printk(KERN_INFO "DMA %s: tw AVSRST _u8 %x :: 0x%x    \n",  dev->name, (AVSRST<<2), regDW );

	reg_writel(DMA_CMD, 0);          // u32
	regDW = reg_readl( DMA_CMD );
	printk(KERN_INFO "DMA %s: tw DMA_CMD _u8 %x :: 0x%x    \n",  dev->name, (DMA_CMD<<2), regDW );

	reg_writel(DMA_CHANNEL_ENABLE,0);
	regDW = reg_readl( DMA_CHANNEL_ENABLE );
	printk(KERN_INFO "DMA %s: tw DMA_CHANNEL_ENABLE %x :: 0x%x    \n",  dev->name, DMA_CHANNEL_ENABLE, regDW );

	regDW = reg_readl( DMA_CHANNEL_ENABLE );
	printk(KERN_INFO "DMA %s: tw DMA_CHANNEL_ENABLE %x :: 0x%x    \n",  dev->name, DMA_CHANNEL_ENABLE, regDW );
	//reg_writel(DMA_CHANNEL_TIMEOUT, 0x180c8F88);   //  860 a00  0x140c8560  0x1F0c8b08     0xF00F00   140c8E08   0x140c8D08
	reg_writel(DMA_CHANNEL_TIMEOUT, 0x3EFF0FF0);     // longer timeout setting
	regDW = reg_readl( DMA_CHANNEL_TIMEOUT );
	printk(KERN_INFO "DMA %s: tw DMA_CHANNEL_TIMEOUT %x :: 0x%x    \n",  dev->name, DMA_CHANNEL_TIMEOUT, regDW );


	reg_writel(DMA_INT_REF, 0x38000);   ///   2a000 2b000 2c000  3932e     0x3032e
	regDW = reg_readl( DMA_INT_REF );
	printk(KERN_INFO "DMA %s: tw DMA_INT_REF %x :: 0x%x    \n",  dev->name, DMA_INT_REF, regDW );


	reg_writel(DMA_CONFIG, 0x00FF0004);
	regDW = reg_readl( DMA_CONFIG );
	printk(KERN_INFO "DMA %s: tw DMA_CONFIG %x :: 0x%x    \n",  dev->name, DMA_CONFIG, regDW );


	regDW = (0xFF << 16) | (VIDEO_GEN_PATTERNS <<8) | VIDEO_GEN;

	printk(KERN_INFO " set tw68 VIDEO_CTRL2 %x :: 0x%x    \n",  VIDEO_CTRL2, regDW );

	reg_writel(VIDEO_CTRL2, regDW);
	regDW = reg_readl( VIDEO_CTRL2 );
	printk(KERN_INFO "DMA %s: tw DMA_CONFIG %x :: 0x%x    \n",  dev->name, VIDEO_CTRL2, regDW );

	//VDelay
	regDW = 0x014;

	reg_writel(VDELAY0, regDW);
	reg_writel(VDELAY1, regDW);
	reg_writel(VDELAY2, regDW);
	reg_writel(VDELAY3, regDW);
	// +4 decoder   0x100
	//   6869
	reg_writel(VDELAY0 +0x100, regDW);
	reg_writel(VDELAY1 +0x100, regDW);
	reg_writel(VDELAY2 +0x100, regDW);
	reg_writel(VDELAY3 +0x100, regDW);
	

	//Show Blue background if no signal
	regDW = 0xe6;
	reg_writel(MISC_CONTROL2, regDW);
	// 6869
	reg_writel(MISC_CONTROL2 +0x100, regDW);



	regDW = reg_readl( VDELAY0 );
	printk(KERN_INFO " read tw68 VDELAY0 %x :: 0x%x    \n",  VDELAY0, regDW );

	regDW = reg_readl( MISC_CONTROL2 );
	printk(KERN_INFO " read tw68 MISC_CONTROL2 %x :: 0x%x    \n",  MISC_CONTROL2, regDW );



	//////////////////////////////////////
	// 2864 #1/External, Muxed-656
	//Reset 2864s

	val1 = reg_readl(CSR_REG);
	val1 &= 0x7FFF;
	reg_writel(CSR_REG, val1);
	/// printk("2864 init CSR_REG 0x%x]= I2C 2864   val1:0X%x  %x\n", CSR_REG,  val1 );

	mdelay(100);
	val1 |= 0x8002;  // Pull out from reset and enable I2S
	reg_writel(CSR_REG, val1);
	/// printk("2864 init CSR_REG 0x%x]=  I2C 2864   val1:0X%x  %x\n", CSR_REG,  val1 );



	addr = CLKOCTRL | 0x100;
	///val0 = DeviceRead2864(dev, addr);
	val1 = 0x40 | ( VIDEO_IN_MODE | (VIDEO_IN_MODE<<2) ) ; // Out enable
	///DeviceWrite2864(dev, addr, (unsigned char)val1);
	///val2=DeviceRead2864(dev, addr);
	/// printk("2864[CLKOCTRL 0x%x]=  I2C 2864  val0:0X%x   val1:0X%x   val2:0X%x\n", CLKOCTRL, val0, val1, val2 );

	addr = NOVID | 0x100;
	val1 = 0x73;					// CHID with 656 Sync code, 656 output even no video input
	///val0 = DeviceRead2864(dev,addr);
	///DeviceWrite2864(dev,addr,val1);
	///val2 = DeviceRead2864(dev,addr);

	// device data structure initialization
	TW68_video_init1(dev);

	// decoder parameter setup
	TW68_video_init2(dev);   // set TV param

	dev->video_DMA_1st_started =0;  // initial value for skipping startup DMA error
	dev->err_times =0;  // DMA error counter

	dev->TCN = 16;

	for (k=0; k<8; k++) {
		dev->vc[k].videoDMA_run=0;
	}

	return 0;
}




/* shutdown */
static int TW68_hwfini(struct TW68_dev *dev)

{
	dprintk("hwfini\n");

	return 0;
}


int vdev_init (struct TW68_dev *dev, struct video_device *template,  char *type);

static void TW68_unregister_video(struct TW68_dev *dev)
{
        int i;
        for (i = 0; i < 8; i++) {
                video_unregister_device(&dev->vc[i].vdev);
        }
        v4l2_device_unregister(&dev->v4l2_dev);
    return;
}

// probe function
static int TW68_initdev(struct pci_dev *pci_dev,
			const struct pci_device_id *pci_id)
{
	struct TW68_dev *dev;
	int err, err0;
	int tmp;
	int i;
    struct pci_dev *pci2;
    int ivytown = 0;
	printk(KERN_INFO "812/1012[vbuf2] driver 05/17/2016 1.1.11\n");

	if (TW68_devcount == TW68_MAXBOARDS)
		return -ENOMEM;

	dev = kzalloc(sizeof(*dev),GFP_KERNEL);
	if (NULL == dev)
		return -ENOMEM;
    // search for ivytown
    pci2 = pci_get_device(0x8086, 0x0e00, NULL);
    if (pci2 != NULL) {
            printk(KERN_INFO "ivytown detected\n");
            ivytown = 1;
            pci_dev_put(pci2);
    }
    pci2 = pci_get_device(0x8086, 0x2f00, NULL);
    if (pci2 != NULL) {
            printk(KERN_INFO "xeon E5/haswell detected\n");
            ivytown = 1;
            pci_dev_put(pci2);
    }
	mutex_init(&dev->lock);
	
	for (i = 0; i < 8; i++) {
		dev->vc[i].dev = dev;
        dev->vc[i].nId = i;
		spin_lock_init(&dev->vc[i].qlock);
		mutex_init(&dev->vc[i].vb_lock);
	}

	err = v4l2_device_register(&pci_dev->dev, &dev->v4l2_dev);
	if (err)
		goto fail0;


	/* pci init */
    dev->ivytown = ivytown;
	dev->pci = pci_dev;
	if (pci_assign_resource(pci_dev,0)) {
		err = -EIO;
		goto fail1;
	}
	if (pci_enable_device(pci_dev)) {
		err = -EIO;
		goto fail1;
	}

	dev->nr = TW68_devcount;
	sprintf(dev->name,"TW%x[%d]",pci_dev->device,dev->nr);

	printk(KERN_INFO " %s TW68_devcount: %d \n", dev->name, TW68_devcount );

	/* pci quirks */
	if (pci_pci_problems) {
		if (pci_pci_problems & PCIPCI_TRITON)
			printk(KERN_INFO "%s: quirk: PCIPCI_TRITON\n", dev->name);
		if (pci_pci_problems & PCIPCI_NATOMA)
			printk(KERN_INFO "%s: quirk: PCIPCI_NATOMA\n", dev->name);
		if (pci_pci_problems & PCIPCI_VIAETBF)
			printk(KERN_INFO "%s: quirk: PCIPCI_VIAETBF\n", dev->name);
		if (pci_pci_problems & PCIPCI_VSFX)
			printk(KERN_INFO "%s: quirk: PCIPCI_VSFX\n",dev->name);
#ifdef PCIPCI_ALIMAGIK
		if (pci_pci_problems & PCIPCI_ALIMAGIK) {
			printk(KERN_INFO "%s: quirk: PCIPCI_ALIMAGIK -- latency fixup\n",
			       dev->name);
			latency = 0x0A;
		}
#endif
		if (pci_pci_problems & (PCIPCI_FAIL|PCIAGP_FAIL)) {
			printk(KERN_INFO "%s: quirk: this driver and your "
			       "chipset may not work together"
			       " in overlay mode.\n",dev->name);
			if (!TW68_no_overlay) {
				printk(KERN_INFO "%s: quirk: overlay "
				       "mode will be disabled.\n",
				       dev->name);
				TW68_no_overlay = 1;
			} else {
				printk(KERN_INFO "%s: quirk: overlay "
				       "mode will be forced. Use this"
				       " option at your own risk.\n",
				       dev->name);
			}
		}
	}

	/* print pci info */
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER,  &dev->pci_lat);

	printk(KERN_INFO "%s: found at %s, rev: %d, irq: %d, "
	       "latency: %d, mmio: 0x%llx\n", dev->name,
	       pci_name(pci_dev), dev->pci_rev, pci_dev->irq,
	       dev->pci_lat,(unsigned long long)pci_resource_start(pci_dev,0));

	pci_set_master(pci_dev);
	pci_set_drvdata(pci_dev, &(dev->v4l2_dev));
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
	if (!pci_dma_supported(pci_dev, DMA_BIT_MASK(32))) {
		printk("%s: Oops: no 32bit PCI DMA ???\n",dev->name);
		err = -EIO;
		goto fail1;
	}
	else
		printk("%s: Hi: 32bit PCI DMA supported \n",dev->name);
#endif

	dev->board = 1;
	printk(KERN_INFO "%s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
	       dev->name,pci_dev->subsystem_vendor,
	       pci_dev->subsystem_device,TW68_boards[dev->board].name,
	       dev->board, dev->autodetected ?
	       "autodetected" : "insmod option");

	/* get mmio */
	if (!request_mem_region(pci_resource_start(pci_dev,0),
				pci_resource_len(pci_dev,0),
				dev->name)) {
		err = -EBUSY;
		printk(KERN_ERR "%s: can't get MMIO memory @ 0x%llx\n",
		       dev->name,(unsigned long long)pci_resource_start(pci_dev,0));
		goto fail1;
	}


	// no cache
	dev->lmmio = ioremap_nocache(pci_resource_start(pci_dev, 0),  pci_resource_len(pci_dev, 0));

	dev->bmmio = (__u8 __iomem *)dev->lmmio;

	if (NULL == dev->lmmio) {
		err = -EIO;
		printk(KERN_ERR "%s: can't ioremap() MMIO memory\n",
		       dev->name);
		goto fail2;
	}
   	//printk(KERN_INFO "	TW6869 PCI_BAR0 mapped registers: phy: 0x%X   dev->lmmio 0X%x  dev->bmmio 0X%x   length: %x \n",
        //  pci_resource_start(pci_dev, 0), (unsigned int)dev->lmmio, (unsigned int)dev->bmmio, (unsigned int)pci_resource_len(pci_dev,0) );
	/* initialize hardware #1 */

	reg_writel(0xfb, 0xa2);
	reg_writel(GPIO_REG, 0xffff0000);
	(void) reg_readl(0xfb);
	reg_writel(GPIO_REG, 0xffff0000);
	msleep(30);
	tmp = reg_readl(GPIO_REG);
	//printk("tmp:  %x\n",tmp);
	tmp = ~tmp&0x07ed; tmp ^= 0x07ed; tmp &= 0x0ff7; 
	if (tmp!=0) {
		tmp|= 0x00ffff03;
		reg_writel(CLKOPOL,tmp);
		reg_writel(SYS_SOFT_RST, 0x01);
		reg_writel(SYS_SOFT_RST, 0x0F);
		printk("s812: device error.\n");
		err = -ENODEV;
		goto fail3;
	}

	TW68_hwinit1(dev);

	/* get irq */
	printk( "TW68_initdev   %s: request IRQ %d\n",     dev->name,pci_dev->irq);

	err = request_irq(pci_dev->irq, TW68_irq,  IRQF_SHARED , dev->name, dev);

	if (err < 0) {
		printk(KERN_ERR "%s: can't get IRQ %d\n",
		       dev->name,pci_dev->irq);
		goto fail3;
	}

	v4l2_prio_init(&dev->prio);

	printk(KERN_ERR "Adding  TW686v_devlist %p\n", &TW686v_devlist);
	list_add_tail(&dev->devlist, &TW686v_devlist);
	////add current TW68_dev device structure node

	/* register v4l devices */
	if (TW68_no_overlay > 0)
		printk(KERN_INFO "%s: Overlay support disabled.\n", dev->name);
	else
		printk(KERN_INFO "%s: Overlay supported %d .\n", dev->name, TW68_no_overlay);

	err0 = vdev_init(dev, &TW68_video_template,"video");    

	if (err0 < 0) {
		printk(KERN_INFO "%s: can't register video device\n",
		       dev->name);
		err = -ENODEV;
		goto fail4;
	}
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 0)
#ifdef CONFIG_PROC_FS
	sprintf(dev->proc_name, "sray_812_%d", (dev->vc[0].vdev.minor / 8));
	dev->tw68_proc = proc_create_data(dev->proc_name, 0, 0, &tw68_procmem_fops, dev);
#endif
#endif

#ifdef AUDIO_BUILD
    for (i = 0; i < TW68_NUM_AUDIO; i++) {
            struct TW68_adev *adev;
            adev = kzalloc(sizeof(struct TW68_adev), GFP_KERNEL);
            if (NULL == adev) {
                return -ENOMEM;
            }
            adev->chip = dev;
            adev->channel_id = i;
            dev->aud_dev[i] = adev;
            if (TW68_alsa_create(adev) < 0) {
                    printk(KERN_ERR "%s:  Failed to create "
                           "audio adapters %d\n", __func__, i);
                    kfree( adev );
                    dev->aud_dev[i] = NULL;
                    break;
            }
    }
#endif
	TW68_devcount++;
	printk(KERN_INFO "%s: registered PCI device %d [v4l2]:%d  err: |%d| \n",
	       dev->name, TW68_devcount, dev->vc[0].vdev.num, err0);

	return 0;
fail4:
	TW68_unregister_video(dev);
	free_irq(pci_dev->irq, dev);
fail3:
	TW68_hwfini(dev);
	if (dev->lmmio) {
		iounmap(dev->lmmio);
		dev->lmmio = NULL;
	}
fail2:
	release_mem_region(pci_resource_start(pci_dev,0),
			   pci_resource_len(pci_dev,0));
fail1:
	v4l2_device_unregister(&dev->v4l2_dev);
fail0:
	kfree(dev);
	printk("probe failed 0x%x\n", err);
	return err;
}

static void TW68_finidev(struct pci_dev *pci_dev)
{
	int m, n = 0;

	struct v4l2_device *v4l2_dev = pci_get_drvdata(pci_dev);
	struct TW68_dev *dev = container_of(v4l2_dev, struct TW68_dev, v4l2_dev);
	printk(KERN_INFO "%s: Starting unregister video device %d\n",
	       dev->name, dev->vc[0].vdev.num);

	printk(KERN_INFO " /* shutdown hardware */ dev 0x%p \n" , dev);


	reg_writel(DMA_CMD, 0);
	reg_writel(DMA_CHANNEL_ENABLE, 0);
	msleep(50);

#ifdef AUDIO_BUILD
	// ALSA free should be first.  We may need to shut off audio
	// and this will send commands to the chip.
	for (m = 0; m < TW68_NUM_AUDIO; m++) {
		if (dev->aud_dev[m]) {
			TW68_alsa_free(dev->aud_dev[m]);
			kfree(dev->aud_dev[m]);
		}
		dev->aud_dev[m] = NULL;
	}
#endif

	TW68_hwfini(dev);
	/* shutdown subsystems */


	/* unregister */
	mutex_lock(&TW68_devlist_lock);
	list_del(&dev->devlist);
	mutex_unlock(&TW68_devlist_lock);
	TW68_devcount--;
	printk(KERN_INFO " list_del(&dev->devlist) \n ");


	/* the DMA sound modules should be unloaded before reaching
	   this, but just in case they are still present... */
	//if (dev->dmasound.priv_data != NULL) {
	// free_irq(pci_dev->irq, &dev->dmasound);
	//	dev->dmasound.priv_data = NULL;
	//}

	del_timer(&dev->delay_resync);

	/* release resources */
	/// remove IRQ
	free_irq(pci_dev->irq, dev);     /////////  0420
	if (dev->lmmio) {
		iounmap(dev->lmmio);
		dev->lmmio = NULL;
	}
	release_mem_region(pci_resource_start(pci_dev,0),
			   pci_resource_len(pci_dev,0));

	TW68_pgtable_free(dev->pci, &dev->m_Page0);
	for (n =0; n<8; n++) {
		for (m =0; m<4; m++)
		{
			if (dev->BDbuf[n][m].cpu == NULL)
				continue;
			pci_free_consistent(dev->pci, 800*300*2, dev->BDbuf[n][m].cpu, dev->BDbuf[n][m].dma_addr);
			dev->BDbuf[n][m].cpu = NULL;
		}
	}

	TW68_unregister_video(dev);


#ifdef CONFIG_PROC_FS
	if (dev->tw68_proc) {
		remove_proc_entry(dev->proc_name, NULL);
		dev->tw68_proc = NULL;
	}
#endif

	printk(KERN_INFO " TW68_unregister_video(dev); \n ");

	printk(KERN_INFO " unregistered v4l2_dev device  %d %d %d\n", 
	       TW68_VERSION_CODE >>16, (TW68_VERSION_CODE >>8)&0xFF, TW68_VERSION_CODE &0xFF);
	/* free memory */
	kfree(dev);
}



/*
 * This function generates an entry in the file system's /proc directory.
 * The information provided here is useful for conveying status
 * and diagnostics info to the outside world.
 * Returns length of output string.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
#ifdef CONFIG_PROC_FS
static int tw68_proc_open(struct inode *inode, struct file *file) 
{
        return single_open(file, tw68_read_show, PDE_DATA(inode));

}
static const struct file_operations tw68_procmem_fops = {
        .open = tw68_proc_open,
        .read = seq_read,
        .llseek = seq_lseek,
        .release = seq_release
};

static int tw68_read_show(struct seq_file *m, void *v)
{
        struct TW68_dev *dev;
        int j;
        /* Generate report heading.*/
        if (m == NULL)
                return 0;
        dev = m->private;
        if (dev == NULL)
                return 0;

        seq_printf(m, "Sensoray 812/2012 drvr debug stats[vbuf2]\n");
        seq_printf(m, "==================================\n");
	seq_printf(m, "dma timeout: %d\n", dev->vc[0].stat.dma_timeout);
	seq_printf(m, "(video errors may be caused by load, signal, or chipset issues)\n");
	for (j = 0; j < 8; j++) {
		seq_printf(m, "[%d] dma_err: %d field_off: %d vbuf_to: %d\n",
			   j, dev->vc[j].stat.dma_err,
			   dev->vc[j].stat.pb_mismatch,
			   dev->vc[j].stat.v4l_timeout);
	}
	for (j = 0; j < 8; j++) {
		/*		seq_printf(m, "[%d] audio mismatches: %d data: %d \n",
			   j, dev->stat_audio[j].pb_mismatch,
			   dev->aud_dev[j]->captured_bytes);*/
	}
	/* Insert a linefeed.*/
	seq_printf(m, "\n");
	
	/* Return char count of the generated string.*/
	return 0;
}
#endif
#endif

/* ----------------------------------------------------------- */
static struct pci_driver TW68_pci_driver = {
	.name = "sray812",
	.id_table = TW68_pci_tbl,
	.probe = TW68_initdev,
	.remove = TW68_finidev,
//#ifdef CONFIG_PM
//	.suspend = TW68_suspend,
//	.resume = TW68_resume
//#endif
};
/* ----------------------------------------------------------- */

static int TW68_init(void)
{
	INIT_LIST_HEAD(&TW686v_devlist);
	printk(KERN_INFO "TW68_: v4l2 driver version %d.%d.%d loaded\n", 
	       TW68_VERSION_CODE >>16, (TW68_VERSION_CODE >>8)&0xFF, TW68_VERSION_CODE &0xFF);

	return pci_register_driver(&TW68_pci_driver);
}

static void TW68_fini(void)
{
	pci_unregister_driver(&TW68_pci_driver);
	printk(KERN_INFO "TW68_: v4l2 driver version %d.%d.%d removed\n", 
	       TW68_VERSION_CODE >>16, (TW68_VERSION_CODE >>8)&0xFF, TW68_VERSION_CODE &0xFF);

}

module_init(TW68_init);
module_exit(TW68_fini);


/* ----------------------------------------------------------- */
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */



