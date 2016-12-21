/*  v4l2 device driver for TW6868 based CVBS PCIe cards  (c)
 *2011,2012 Simon Xu @ Intersil 
 */
// VIDEOBUF2 code and improvements Copyright Sensoray Company Inc.
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/i2c.h>
#include <linux/videodev2.h>
#include <linux/kdev_t.h>
#include <linux/input.h>
#include <linux/notifier.h>
#include <linux/delay.h>
#include <linux/mutex.h>

#include <asm/io.h>
#include <linux/kthread.h>
#include <linux/highmem.h>
#include <linux/freezer.h>


#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/tuner.h>
////#include <media/ir-common.h>
////#include <media/ir-kbd-i2c.h>
#include <media/videobuf-dma-sg.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <media/videobuf2-vmalloc.h>

#define TW68_VERSION_CODE KERNEL_VERSION(2, 3, 13)

#define DPRT_LEVEL0 2
extern unsigned int TW68_debug_alsa;
#define daprintk(level, dev, fmt, arg...)\
	do { if (TW68_debug_alsa >= level)\
		printk(KERN_DEBUG "%s[a-%d]: " fmt, ((dev)->chip)->name, (dev)->channel_id, ## arg);\
	} while (0)


#define TW68_NUM_AUDIO 8
#define UNSET (-1U)

/* ----------------------------------------------------------- */
/* enums                                                       */

enum TW68_tvaudio_mode {
	TVAUDIO_FM_MONO       = 1,
	TVAUDIO_FM_BG_STEREO  = 2,
	TVAUDIO_FM_SAT_STEREO = 3,
	TVAUDIO_FM_K_STEREO   = 4,
	TVAUDIO_NICAM_AM      = 5,
	TVAUDIO_NICAM_FM      = 6,
};

enum TW68_audio_in {
	TV    = 1,
	LINE1 = 2,
	LINE2 = 3,
	LINE3 = 4,
	LINE4 = 5,
	LINE2_LEFT,
};

/* ----------------------------------------------------------- */
/* static data                                                 */

struct TW68_tvnorm {
	char          *name;
	v4l2_std_id   id;

	/* video decoder */
	unsigned int  sync_control;
	unsigned int  luma_control;
	unsigned int  chroma_ctrl1;
	unsigned int  chroma_gain;
	unsigned int  chroma_ctrl2;
	unsigned int  vgate_misc;

	/* video scaler */
	unsigned int  h_start;
	unsigned int  h_stop;
	unsigned int  video_v_start;
	unsigned int  video_v_stop;
	unsigned int  vbi_v_start_0;
	unsigned int  vbi_v_stop_0;
	unsigned int  src_timing;
	unsigned int  vbi_v_start_1;
};

struct TW68_tvaudio {
	char         *name;
	v4l2_std_id  std;
	enum         TW68_tvaudio_mode mode;
	int          carr1;
	int          carr2;
};

struct TW68_format {
	char           *name;
	unsigned int   fourcc;
	unsigned int   depth;
	unsigned int   pm;
	unsigned int   vshift;   /* vertical downsampling (for planar yuv) */
	unsigned int   hshift;   /* horizontal downsampling (for planar yuv) */
	unsigned int   bswap:1;
	unsigned int   wswap:1;
	unsigned int   yuv:1;
	unsigned int   planar:1;
	unsigned int   uvswap:1;
};

/* ----------------------------------------------------------- */
/* card configuration                                          */

#define TW68_BOARD_UNKNOWN           0
#define TW68_BOARD_A                 1


#define TW68_MAXBOARDS 4
#define TW68_INPUT_MAX 4

#define PAL_default_width 704
#define NTSC_default_width 704

#define PAL_default_height 576
#define NTSC_default_height 480

/* ----------------------------------------------------------- */
/* Since we support 2 remote types, lets tell them apart       */

#define TW68_REMOTE_GPIO  1
#define TW68_REMOTE_I2C   2

/* ----------------------------------------------------------- */
/* Video Output Port Register Initialization Options           */

#define SET_T_CODE_POLARITY_NON_INVERTED	(1 << 0)
#define SET_CLOCK_NOT_DELAYED			(1 << 1)
#define SET_CLOCK_INVERTED			(1 << 2)
#define SET_VSYNC_OFF				(1 << 3)

struct TW68_input {
	char                    *name;
	unsigned int            vmux;
	enum TW68_audio_in   amux;
	unsigned int            gpio;
	unsigned int            tv:1;
};


struct TW68_board {
	char                    *name;
	unsigned int            audio_clock;

	/* input switching */
	unsigned int            gpiomask;
	struct TW68_input    inputs[TW68_INPUT_MAX];
	struct TW68_input    mute;

	/* i2c chip info */
	unsigned int            tuner_type;
	unsigned int		radio_type;
	unsigned int		radio_addr;
	unsigned char		tuner_addr;
};


/* ----------------------------------------------------------- */
/* device / file handle status                                 */

#define RESOURCE_OVERLAY       1
#define RESOURCE_VIDEO         2
#define RESOURCE_VBI           4

#define INTERLACE_AUTO         0
#define INTERLACE_ON           1
#define INTERLACE_OFF          2

#define BUFFER_TIMEOUT     msecs_to_jiffies(500)  /* 0.5 seconds */
#define TS_BUFFER_TIMEOUT  msecs_to_jiffies(1000)  /* 1 second */

#define RINGSIZE	8

struct TW68_dev;

/* TW686_ page table */
struct TW68_pgtable {
	unsigned int               size;
	__le32                     *cpu;
	dma_addr_t                 dma;
};


/* tvaudio thread status */
struct TW68_thread {
	struct task_struct         *thread;
	unsigned int               scan1;
	unsigned int               scan2;
	unsigned int               mode;
	unsigned int		   stopped;
};

/* buffer for one video/vbi/ts frame */
struct TW68_buf {
	/* common v4l buffer stuff -- must be first */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
	struct vb2_buffer vb;
#else
	struct vb2_v4l2_buffer vb;
#endif
	struct list_head list;
};

#if 0
struct TW68_dmaqueue {
	struct TW68_dev         *dev;
	struct TW68_buf         *curr;
	struct list_head           active;
	struct list_head           queued;
	struct timer_list          timeout;
	unsigned int               DMA_nCH;   /// 
	unsigned int               FieldPB;   /// Top Bottom status, field copy order;
	unsigned int               FCN;   /// 
	struct timer_list          restarter;
};
#endif




/**
 * struct dma_region - large non-contiguous DMA buffer
 * @virt:        kernel virtual address
 * @dev:         PCI device
 * @n_pages:     number of kernel pages
 * @n_dma_pages: number of IOMMU pages
 * @sglist:      IOMMU mapping
 * @direction:   PCI_DMA_TODEVICE, etc.
 *
 * a large, non-physically-contiguous DMA buffer with streaming, asynchronous
 * usage characteristics
 */

struct DMA_Descriptor {
        u32 control;
        u32 address;
};

struct dma_region {
	unsigned char *kvirt;
	struct pci_dev *dev;
	unsigned int n_pages;
	unsigned int n_dma_pages;
	struct scatterlist *sglist;
	int direction;
};

struct video_ctrl {
	int                        ctl_bright;
	int                        ctl_contrast;
	int                        ctl_hue;
	int                        ctl_saturation;
	int                        ctl_freq;
	int                        ctl_mute;             /* audio */
	int                        ctl_volume;
	//	int                        ctl_y_odd;
	//int                        ctl_y_even;
	//int                        ctl_automute;
};

struct dma_mem{
	__le32      *cpu;
	dma_addr_t   dma_addr;
};

struct TW68_adev;
struct TW68_dev;

struct TW68_stat {
	int pb_mismatch;
	int dma_err;
	int dma_timeout; //all channels
	int v4l_timeout; // buffer timeout
};

/* video channel */
struct TW68_vc {
	struct TW68_stat stat;
	spinlock_t slock;
	struct video_device vdev;
	enum v4l2_field field;
	spinlock_t qlock;
	struct mutex vb_lock;
	unsigned int nVideoFormat;
	unsigned int vfd_DMA_num;;
	struct TW68_tvnorm *tvnormf;
	//struct TW68_dmaqueue video_dmaq;
	unsigned int framecount;
	unsigned int PAL50;
	struct video_ctrl video_param;
	int Done; 
	int curPB;;
        struct file *in_use_by;; 

	unsigned int videoDMA_run;
	unsigned int videoDecoderS;
	u64 errlog;   // latest errors jiffies
	struct vb2_queue vb_vidq;
	//struct vb2_queue vb_vbiq;
	/* video capture */
	struct TW68_format        *fmt;
	unsigned int               width,height;
	/* video overlay */
	struct TW68_dev         *dev;
	struct v4l2_window         win;
	struct v4l2_clip           clips[8];
	unsigned int               nclips;
	//set default video standard and frame size
	unsigned int    dW, dH;  // default width hight
	int nId;
	/* vbi capture */
	struct list_head buf_list;
	struct v4l2_ctrl_handler hdl;
	//struct TW68_pgtable     pt_vbi;	
	int viddetected;
};

/* global device status */
struct TW68_dev {
	struct TW68_stat stat_audio[8];
	int ivytown;
#ifdef CONFIG_PROC_FS
	char proc_name[32];
	struct proc_dir_entry *tw68_proc;
#else
	void *tw68_proc;
#endif

	struct list_head           devlist;
	struct mutex               lock;
	struct mutex               start_lock;
	spinlock_t                 slock;

	struct v4l2_prio_state     prio;
	struct v4l2_device         v4l2_dev;
	struct dma_mem BDbuf[8][4];


        // to get rid of static Done in TW68_irq_video_done, which is not correct (multiple channels)

	/* workstruct for loading modules */
	struct work_struct request_module_wk;

	/* insmod option/autodetected */
	int                        autodetected;

	/* various device info */
	unsigned int              video_opened;
	int              video_DMA_1st_started;
	int				 err_times;			// DMA errors counter

	//	unsigned int 			   deadbeef[9];
	struct timer_list          delay_resync;
	unsigned int               resources[16];
	//	struct video_device        *video_dev;
	struct TW68_vc vc[8];



	struct video_device        *radio_dev;
	struct video_device        *vbi_dev;
	struct TW68_adev         *aud_dev[TW68_NUM_AUDIO];
	/// DMA smart control
	unsigned int				videoDMA_ID;
	unsigned int				videoCap_ID;
	unsigned int				videoRS_ID;
	



	unsigned int				TCN;
	unsigned int				skip;

	/* infrared remote */
	int                        has_remote;
	struct card_ir				*remote;

	/* pci i/o */
	char                       name[32];
	int                        nr;
	struct pci_dev             *pci;
	unsigned char              pci_rev,pci_lat;
	__u32                      __iomem *lmmio;
	__u8                       __iomem *bmmio;

//  2010
//  allocate common buffer for DMA entry tables  SG buffer P&B
    struct TW68_pgtable             m_Page0;
    struct TW68_pgtable             m_Page1;

	//    struct TW68_pgtable             m_AudioBuffer;

	/* config info */
	unsigned int               board;
	unsigned int               tuner_type;

	/* video+ts+vbi capture */
	unsigned int 			   QFbit;    // Quad Frame interrupt bits


	//unsigned int               vbi_fieldcount;

	/* various v4l controls */
	struct TW68_tvnorm      *tvnorm;              /* video */


	struct TW68_tvaudio     *tvaudio;

	unsigned int               ctl_input;
	int                        ctl_bright;
	int                        ctl_contrast;
	int                        ctl_hue;
	int                        ctl_saturation;
	int                        ctl_freq;
	int                        ctl_mute;             /* audio */
	int                        ctl_volume;
	int                        ctl_y_odd;
	int                        ctl_y_even;
	int                        ctl_automute;




	/* crop */
	struct v4l2_rect           crop_bounds;
	struct v4l2_rect           crop_defrect;
	struct v4l2_rect           crop_current;

	/* other global state info */
	int pcount; //print count

	wait_queue_head_t dma_wq;
	int last_dmaerr;
};

/*
 * audio main chip structure
 */
//	struct pci_dev *pci;


/* dmasound dsp status */
struct TW68_adev {
	int snd_buffer_pos;
	int snd_period_pos;

	struct TW68_dev* chip;
	int                        channel_id;

	struct mutex               lock;

	dma_addr_t dma_addr;
	unsigned char *dma_area;
	struct snd_pcm_substream   *substream;
	atomic_t snd_stream;
	struct snd_card *snd;
	struct work_struct snd_trigger;
	unsigned int               pb_flag;
};

/* ----------------------------------------------------------- */


#define tw_read(reg)             readl(chip->bmmio + (reg))

#define tw_write(reg, value)     {writel((value), chip->bmmio + (reg));\
		if (chip->ivytown)				       \
			udelay(10);				       \
		readl(chip->bmmio + 4*PHASE_REF_CONFIG);	       \
		if (chip->ivytown)				       \
			udelay(25); }

#define reg_readl(reg)             readl(dev->lmmio + (reg))

#define reg_writel(reg,value)      {writel((value), dev->lmmio + (reg)); \
		if (dev->ivytown)					\
			udelay(10);					\
		readl(dev->lmmio + PHASE_REF_CONFIG);			\
		if (dev->ivytown)					\
			udelay(25); }



#define TW68_NORMS	(\
		V4L2_STD_PAL    | V4L2_STD_PAL_N | \
		V4L2_STD_PAL_Nc | V4L2_STD_SECAM | \
		V4L2_STD_NTSC   | V4L2_STD_PAL_M | \
		V4L2_STD_PAL_60)

/* ----------------------------------------------------------- */
/* TW68-core.c                                                 */

#define _PGTABLE_SIZE 4096

extern struct list_head  TW686v_devlist;
extern struct mutex  TW686v_devlist_lock;

extern int TW68_no_overlay;

//void TW68_track_gpio(struct TW6864_dev *dev, char *msg);
//void TW68_set_gpio(struct TW6864_dev *dev, int bit_no, int value);

///void GPIOI2C(struct TW68_dev *dev, unsigned int GPIOaddr);
///unsigned char DeviceRead2864(struct TW68_dev *dev, unsigned long  byte_addr);
///void DeviceWrite2864(struct TW68_dev *dev, unsigned long byte_addr, unsigned char value);

void tw68v_set_framerate(struct TW68_dev *dev, u32 ch, u32 n);

int videoDMA_pgtable_alloc(struct pci_dev *pci, struct TW68_pgtable *pt);
int AudioDMA_PB_alloc(struct pci_dev *pci, struct TW68_pgtable *pt);

/// struct pci_dev *pci,  TW68
int  _pgtable_build( struct TW68_pgtable *pt,
			   struct scatterlist *list, unsigned int length,
			   unsigned int startpage, unsigned int size);
void _pgtable_free(struct pci_dev *pci, struct TW68_pgtable *pt);

int _buffer_count(unsigned int size, unsigned int count);
int _buffer_startpage(struct TW68_buf *buf);
unsigned long TW68_buffer_base(struct TW68_buf *buf);
int TW68_buffer_pages(int size);

#if 0
int TW68_buffer_queue(struct TW68_dev *dev, struct TW68_dmaqueue *q, struct TW68_buf *buf);
void TW68_buffer_finish(struct TW68_dev *dev, struct TW68_dmaqueue *q, unsigned int state);
void TW68_buffer_next(struct TW68_dev *dev, struct TW68_dmaqueue *q);
int TW68_buffer_requeue(struct TW68_dev *dev, struct TW68_dmaqueue *q);
#endif

void DecoderResize(struct TW68_dev *dev, int nId, int H, int W); 
void Field_SG_Mapping(struct TW68_dev *dev, int field_PB);
void Fixed_SG_Mapping(struct TW68_dev *dev, int nDMA_channel, int Frame_size);  //    0 1
void BFDMA_setup(struct TW68_dev *dev, int nDMA_channel, int H, int W);

int  Field_Copy(struct TW68_dev *dev, int nDMA_channel, int field_PB);
int  BF_Copy(struct TW68_dev *dev, int nDMA_channel, u32 Fn, u32 PB, u32 single, struct TW68_buf *buf);
int  QF_Field_Copy(struct TW68_dev *dev, int nDMA_channel, u32 Fn, u32 PB);

void DMAstarter(unsigned long data);
void RestartDMA(unsigned long data);
void resync(unsigned long data);
void TW68_buffer_timeout(unsigned long data);
void TW68_dma_free(struct videobuf_queue *q,struct TW68_buf *buf);

int TW68_set_dmabits(struct TW68_dev *dev,  unsigned int DMA_nCH);
int	stop_video_DMA(struct TW68_dev *dev, unsigned int DMA_nCH);
int	Hardware_reset(struct TW68_dev *dev);
int	VideoDecoderDetect(struct TW68_dev *dev, unsigned int DMA_nCH);

extern int (*TW68_dmasound_init)(struct TW68_dev *dev);
extern int (*TW68_dmasound_exit)(struct TW68_dev *dev);

u64 GetDelay(struct TW68_dev *dev, int no);

/* ----------------------------------------------------------- */
extern struct TW68_board TW68_boards[];
extern const unsigned int TW68_bcount;
extern struct pci_device_id TW68_pci_tbl[];



/* ----------------------------------------------------------- */
/* TW68-i2c.c                                               */

int TW68_i2c_register(struct TW68_dev *dev);
int TW68_i2c_unregister(struct TW68_dev *dev);


/* ----------------------------------------------------------- */
/* TW68-video.c                                             */

extern unsigned int video_debug;
extern struct video_device TW68_video_template;
//extern struct video_device TW68_radio_template;


/* ----------------------------------------------------------- */
/* TW68-alsa.c                                              */
/* Audio */
extern int  TW68_alsa_create(struct TW68_adev *adev);
extern int  TW68_alsa_free(struct TW68_adev *adev);

extern void TW68_alsa_irq(struct TW68_adev *dev, u32 dma_status, u32 pb_status);
//extern void TW68_alsa_irq(struct TW68_dev *dev, u32 dma_status, u32 pb_status);
//extern int  TW68_alsa_resetdma(struct TW68_chip *chip, u32 dma_cmd);

/* ----------------------------------------------------------- */
/* TW68-audio.c                                              */
/* Audio */
extern int  TW68_audio_create(struct TW68_dev *dev);
extern int  TW68_audio_free(struct TW68_dev *dev);
extern void TW68_audio_irq(struct TW68_dev *dev, u32 dma_status, u32 pb_status);
//extern int  TW68_audio_param(struct TW68_dev *dev, struct TW68_aparam* ap);
//extern int  TW68_audio_data(struct TW68_dev *dev, struct TW68_adata* ap);
//extern int  TW68_audio_resetdma(struct TW68_chip *chip, u32 dma_cmd);
//extern int  TW68_audio_trigger(struct TW68_dev *dev, int cmd);



int TW68_s_ctrl_internal(struct TW68_vc *vc, struct v4l2_control *c);
int TW68_g_ctrl_internal(struct TW68_vc *vc, struct v4l2_control *c);
int TW68_queryctrl(struct file *file, void *priv, struct v4l2_queryctrl *c);
int TW68_s_std_internal(struct TW68_vc *vc, v4l2_std_id *id);

int TW68_videoport_init(struct TW68_dev *dev);
// void TW68_set_tvnorm_hw(struct TW68_dev *dev);

int TW68_video_init1(struct TW68_dev *dev);
int TW68_video_init2(struct TW68_dev *dev);
void TW68_irq_video_signalchange(struct TW68_dev *dev);
void TW68_irq_video_done(struct TW68_dev *dev, unsigned int nId, u32 dwRegPB);

int buffer_setup(struct videobuf_queue *q, unsigned int *count, unsigned int *size);
int buffer_setup_QF(struct videobuf_queue *q, unsigned int *count, unsigned int *size);


/* TW68-tvaudio.c                                           */

int TW68_tvaudio_rx2mode(u32 rx);

void TW68_tvaudio_setmute(struct TW68_dev *dev);
void TW68_tvaudio_setinput(struct TW68_dev *dev, struct TW68_input *in);
void TW68_tvaudio_setvolume(struct TW68_dev *dev, int level);
int TW68_tvaudio_getstereo(struct TW68_dev *dev);

void TW68_tvaudio_init(struct TW68_dev *dev);
int TW68_tvaudio_init2(struct TW68_dev *dev);
int TW68_tvaudio_fini(struct TW68_dev *dev);
int TW68_tvaudio_do_scan(struct TW68_dev *dev);

//void TW68_enable_i2s(struct TW68_dev *dev);

/* ----------------------------------------------------------- */

extern const struct file_operations TW68_dsp_fops;
extern const struct file_operations TW68_mixer_fops;

/* ----------------------------------------------------------- */
/* TW68-input.c                                             */

int  TW68_input_init1(struct TW68_dev *dev);
void TW68_input_fini(struct TW68_dev *dev);
void TW68_input_irq(struct TW68_dev *dev);
void TW68_probe_i2c_ir(struct TW68_dev *dev);
void TW68_ir_start(struct TW68_dev *dev, struct card_ir *ir);
void TW68_ir_stop(struct TW68_dev *dev);


//
int buffer_resetup(struct TW68_dev *dev, u32 devid, struct videobuf_buffer *buf);

#define TW_AUDIO_BUFFER 65536
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
