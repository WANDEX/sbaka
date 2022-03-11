/*
 * kernel: 5.4.17
 * Simple disk driver.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>
#include <linux/errno.h>	/* error codes */

#include <linux/timer.h>
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>


MODULE_DESCRIPTION("Very bad module, do not dare to use any of its code in your work");
MODULE_LICENSE("Dual BSD/GPL");


static int sbaka_major = 0;
module_param(sbaka_major, int, 0);
static int hardsect_size = 512;
module_param(hardsect_size, int, 0);
static int nsectors = 1024;	/* How big the drive is */
module_param(nsectors, int, 0);
static int ndevices = 4;
module_param(ndevices, int, 0);

/*
 * The different "request modes" we can use.
 */
enum {
	RM_SIMPLE  = 0,	/* The extra-simple request function */
	RM_FULL    = 1,	/* The full-blown version */
	RM_NOQUEUE = 2,	/* Use make_request */
};
static int request_mode = RM_SIMPLE;
module_param(request_mode, int, 0);

/*
 * Minor number and partition management.
 */
#define SBAKA_MINORS	16
#define MINOR_SHIFT	4
#define DEVNUM(kdevnum)	(MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE	512

/*
 * After this much idle time, the driver will simulate a media change.
 */
#define INVALIDATE_DELAY	30*HZ

/*
 * The internal representation of our device.
 */
struct sbaka_dev {
        int size;                       /* Device size in sectors */
        u8 *data;                       /* The data array */
        short users;                    /* How many users */
        short media_change;             /* Flag a media change? */
        spinlock_t lock;                /* For mutual exclusion */
        struct request_queue *queue;    /* The device request queue */
        struct gendisk *gd;             /* The gendisk structure */
        struct timer_list timer;        /* For simulated media changes */
};

static struct sbaka_dev *Devices = NULL;


/*
 * See https://github.com/openzfs/zfs/pull/10187/
 */
static inline struct request_queue *
blk_generic_alloc_queue(make_request_fn make_request, int node_id)
{
	struct request_queue *q = blk_alloc_queue(GFP_KERNEL);
	if (q != NULL)
		blk_queue_make_request(q, make_request);
	return (q);
}

/*
 * Handle an I/O request.
 */
static void sbaka_transfer(struct sbaka_dev *dev, unsigned long sector,
			   unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector*KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect*KERNEL_SECTOR_SIZE;

	if ((offset + nbytes) > dev->size) {
		printk(KERN_NOTICE "Beyond-end write (%ld %ld)\n", offset, nbytes);
		return;
	}
	if (write)
		memcpy(dev->data + offset, buffer, nbytes);
	else
		memcpy(buffer, dev->data + offset, nbytes);
}

/*
 * Transfer a single BIO.
 */
static int sbaka_xfer_bio(struct sbaka_dev *dev, struct bio *bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	sector_t sector = bio->bi_iter.bi_sector;

	/* Do each segment independently. */
	bio_for_each_segment(bvec, bio, iter) {
		char *buffer = kmap_atomic(bvec.bv_page) + bvec.bv_offset;
		sbaka_transfer(dev, sector, (bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE),
				buffer, bio_data_dir(bio) == WRITE);
		sector += (bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE);
		kunmap_atomic(buffer);
	}
	return 0; /* Always "succeed" */
}

/*
 * The direct make request.
 */
static blk_qc_t sbaka_make_request(struct request_queue *q, struct bio *bio)
{
	struct sbaka_dev *dev = bio->bi_disk->private_data;
	int status;

	status = sbaka_xfer_bio(dev, bio);
	bio->bi_status = status;
	bio_endio(bio);
	return BLK_QC_T_NONE;
}

/*
 * Open and close.
 */

static int sbaka_open(struct block_device *bdev, fmode_t mode)
{
	struct sbaka_dev *dev = bdev->bd_disk->private_data;

	del_timer_sync(&dev->timer);
	spin_lock(&dev->lock);
	if (! dev->users)
		check_disk_change(bdev);
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

static void sbaka_release(struct gendisk *disk, fmode_t mode)
{
	struct sbaka_dev *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;

	if (!dev->users) {
		dev->timer.expires = jiffies + INVALIDATE_DELAY;
		add_timer(&dev->timer);
	}
	spin_unlock(&dev->lock);
}

/*
 * Look for a (simulated) media change.
 */
int sbaka_media_changed(struct gendisk *gd)
{
	struct sbaka_dev *dev = gd->private_data;

	return dev->media_change;
}

/*
 * Revalidate.  WE DO NOT TAKE THE LOCK HERE, for fear of deadlocking
 * with open.  That needs to be reevaluated.
 */
int sbaka_revalidate(struct gendisk *gd)
{
	struct sbaka_dev *dev = gd->private_data;

	if (dev->media_change) {
		dev->media_change = 0;
		memset (dev->data, 0, dev->size);
	}
	return 0;
}

/*
 * The "invalidate" function runs out of the device timer; it sets
 * a flag to simulate the removal of the media.
 */
void sbaka_invalidate(struct timer_list *ldev)
{
	struct sbaka_dev *dev = from_timer(dev, ldev, timer);

	spin_lock(&dev->lock);
	if (dev->users || !dev->data)
		printk (KERN_WARNING "sbaka: timer sanity check failed\n");
	else
		dev->media_change = 1;
	spin_unlock(&dev->lock);
}

/*
 * The ioctl() implementation
 */

// int sbaka_ioctl (struct inode *inode, struct file *filp,
//                  unsigned int cmd, unsigned long arg)
int sbaka_ioctl (struct block_device *bdev, fmode_t mode,
                 unsigned int cmd, unsigned long arg)
{
	long size;
	struct hd_geometry geo;
	struct sbaka_dev *dev = bdev->bd_disk->private_data;

	switch(cmd) {
	    case HDIO_GETGEO:
		/*
		 * Get geometry: since we are a virtual device, we have to make
		 * up something plausible.  So we claim 16 sectors, four heads,
		 * and calculate the corresponding number of cylinders.  We set the
		 * start of data at sector four.
		 */
		size = dev->size*(hardsect_size/KERNEL_SECTOR_SIZE);
		geo.cylinders = (size & ~0x3f) >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	}

	return -ENOTTY; /* unknown command */
}


/*
 * The device operations structure.
 */
static struct block_device_operations sbaka_ops = {
	.owner		 = THIS_MODULE,
	.open		 = sbaka_open,
	.release	 = sbaka_release,
	.media_changed   = sbaka_media_changed,
	.revalidate_disk = sbaka_revalidate,
	.ioctl	         = sbaka_ioctl
};


/*
 * Set up our internal device.
 */
static void setup_device(struct sbaka_dev *dev, int which)
{
	/*
	 * Get some memory.
	 */
	memset (dev, 0, sizeof (struct sbaka_dev));
	dev->size = nsectors*hardsect_size;
	dev->data = vmalloc(dev->size);
	if (dev->data == NULL) {
		printk (KERN_NOTICE "vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);

	/*
	 * The timer which "invalidates" the device.
	 */
	timer_setup(&dev->timer, sbaka_invalidate, 0);

	/*
	 * The I/O queue, depending on whether we are using our own
	 * make_request function or not.
	 */
	dev->queue = blk_generic_alloc_queue(sbaka_make_request, NUMA_NO_NODE);
	if (dev->queue == NULL)
		goto out_vfree;
	blk_queue_logical_block_size(dev->queue, hardsect_size);
	dev->queue->queuedata = dev;
	/*
	 * And the gendisk structure.
	 */
	dev->gd = alloc_disk(SBAKA_MINORS);
	if (! dev->gd) {
		printk (KERN_NOTICE "alloc_disk failure\n");
		goto out_vfree;
	}
	dev->gd->major = sbaka_major;
	dev->gd->first_minor = which*SBAKA_MINORS;
	dev->gd->fops = &sbaka_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf (dev->gd->disk_name, 32, "sbaka%c", which + 'a');
	set_capacity(dev->gd, nsectors*(hardsect_size/KERNEL_SECTOR_SIZE));
	add_disk(dev->gd);
	return;

  out_vfree:
	if (dev->data)
		vfree(dev->data);
}

static int __init sbaka_init(void)
{
	int i;
	/*
	 * Get registered.
	 */
	sbaka_major = register_blkdev(sbaka_major, "sbaka");
	if (sbaka_major <= 0) {
		printk(KERN_WARNING "sbaka: unable to get major number\n");
		return -EBUSY;
	}
	/*
	 * Allocate the device array, and initialize each one.
	 */
	Devices = kmalloc(ndevices*sizeof (struct sbaka_dev), GFP_KERNEL);
	if (Devices == NULL)
		goto out_unregister;
	for (i = 0; i < ndevices; i++)
		setup_device(Devices + i, i);

	printk(KERN_INFO "sbaka: blkdev registered!\n");
	return 0;

  out_unregister:
	unregister_blkdev(sbaka_major, "sbd");  // FIXME: why 'sbd' is in original? typo??
	printk(KERN_INFO "sbaka: blkdev unregistered!\n");
	return -ENOMEM;
}

static void sbaka_exit(void)
{
	int i;
	for (i = 0; i < ndevices; i++) {
		struct sbaka_dev *dev = Devices + i;

		del_timer_sync(&dev->timer);
		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if (dev->queue) {
			if (request_mode == RM_NOQUEUE)
				blk_put_queue(dev->queue);
			else
				blk_cleanup_queue(dev->queue);
		}
		if (dev->data)
			vfree(dev->data);
	}
	unregister_blkdev(sbaka_major, "sbaka");
	printk(KERN_INFO "sbaka: blkdev unregistered!\n");
	kfree(Devices);
}

module_init(sbaka_init);
module_exit(sbaka_exit);
