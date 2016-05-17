#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/hdreg.h>
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/crypto.h>

MODULE_LICENSE("Dual BSD/GPL");

static int enc_ram_major = 0;
module_param(enc_ram_major, int, 0);
static int hardsect_size = 512;
module_param(hardsect_size, int, 0);
static int nsectors = 1024;	//drive size
module_param(nsectors, int, 0);
static int ndevices = 4; 
module_param(ndevices, int, 0);

//request modes
enum {
	RM_SIMPLE = 0,		//simple
	RM_FULL = 1,		//full
	RM_NOQUEUE = 2,		//immediately call make request function
};
static int request_mode = RM_SIMPLE;
module_param(request_mode, int, 0);

#define ENC_RAM_MINORS 16
#define MINOR_SHIFT 4
#define DEVNUM(kdevnum) (MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT
#define KERNEL_SECTOR_SIZE 512

//timeout macro
#define INVALIDATE_DELAY 30*HZ

//actual device vars
struct enc_ram_dev {
	int size;		//in sectors
	u8 *data;		//data array
	short users;
	short media_change;	//media change flag
	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *gd;	//gendisk structure
	struct timer_list timer;	//to track timeouts
};

static struct enc_ram_dev *Devices = NULL;

static char *key = "1537926480";
static int keylen = 10;
struct crypto_cipher *tfm;

static void data_view(unsigned char *data, unsigned int len)
{
	while (len--){
		printk("%02x", *data++);
	}
	printk("\n");
}

//Handle a request (in sectors)
static void enc_ram_transfer(struct enc_ram_dev *dev, unsigned long sector,
	       unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector *KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect *KERNEL_SECTOR_SIZE;
	int i;

	u8 *dst;	//destination
	u8 *src;	//source

	crypto_cipher_setkey(tfm, key, keylen); 


	if ((offset + nbytes) > dev->size) {
		printk(KERN_NOTICE "Attempting to access invalid address (%ld %ld)\n", offset,
		       nbytes);
		return;
	}
	
	if (write){

		// memcpy(dev->data + offset, buffer, nbytes);

		dst = dev->data + offset;
		src = buffer;

		printk("~~~~BEGIN WRITE~~~~\n");
		printk("Before:\n");
		data_view(src, nbytes); 

		for (i = 0; i < nbytes; i += crypto_cipher_blocksize(tfm)) {
			//Encrypt, send to dst
			crypto_cipher_encrypt_one(tfm, dst + i, src + i);
		}

		printk("After:\n");
		data_view(dst, nbytes); 
		printk("~~~~END WRITE~~~~\n");

	} else {

		// memcpy(buffer, dev->data + offset, nbytes);

		dst = buffer;
		src = dev->data + offset;

		printk("~~~~BEGIN READ~~~~\n");
		printk("Before:\n");
		data_view(src, nbytes);

		for (i = 0; i < nbytes; i += crypto_cipher_blocksize(tfm)) {
			//Decrypt, send to dst
			crypto_cipher_decrypt_one(tfm, dst + i, src + i);
		}

		printk("After:\n");
		data_view(dst, nbytes);
		printk("~~~~END READ~~~~\n");
	}
}


//The simple request function
static void enc_ram_request(struct request_queue *q)
{
	struct request *req;

	req = blk_fetch_request(q);
	while (req != NULL) {
		struct enc_ram_dev *dev = req->rq_disk->private_data;
		if (req->cmd_type != REQ_TYPE_FS) {
			printk(KERN_NOTICE "Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		enc_ram_transfer(dev, blk_rq_pos(req),
			       blk_rq_cur_sectors(req), req->buffer,
			       rq_data_dir(req));
		// end_request(req, 1);
		if (!__blk_end_request_cur(req, 0)) {
			req = blk_fetch_request(q);
		}
	}
}


//Transfer a single BIO
static int enc_ram_xfer_bio(struct enc_ram_dev *dev, struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	sector_t sector = bio->bi_sector;

	//Do each segment independently
	bio_for_each_segment(bvec, bio, i) {
		char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
		enc_ram_transfer(dev, sector, bio_cur_bytes(bio) >> 9	// in sectors
			       , buffer, bio_data_dir(bio) == WRITE);
		sector += bio_cur_bytes(bio) >> 9;	//in sectors
		__bio_kunmap_atomic(bio, KM_USER0);
	}
	return 0;
}


//Transfer a full request
static int enc_ram_xfer_request(struct enc_ram_dev *dev, struct request *req)
{
	struct bio *bio;
	int nsect = 0;

	__rq_for_each_bio(bio, req) {
		enc_ram_xfer_bio(dev, bio);
		nsect += bio->bi_size / KERNEL_SECTOR_SIZE;
	}
	return nsect;
}


//The full request function
static void enc_ram_full_request(struct request_queue *q)
{
	struct request *req;
	int sectors_xferred;
	struct enc_ram_dev *dev = q->queuedata;

	req = blk_fetch_request(q);
	while (req != NULL) {
		if (req->cmd_type != REQ_TYPE_FS) {
			printk(KERN_NOTICE "Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		sectors_xferred = enc_ram_xfer_request(dev, req);
		if (!__blk_end_request_cur(req, 0)) {
			blk_fetch_request(q);
		}
	}
}

//The manual make request function
static int enc_ram_make_request(struct request_queue *q, struct bio *bio)
{
	struct enc_ram_dev *dev = q->queuedata;
	int status;
	status = enc_ram_xfer_bio(dev, bio);
	bio_endio(bio, status);
	return 0;
}


//Open and close
static int enc_ram_open(struct block_device *device, fmode_t mode)
{
	struct enc_ram_dev *dev = device->bd_disk->private_data;

	del_timer_sync(&dev->timer);
	/* filp->private_data = dev; */
	spin_lock(&dev->lock);
	if (!dev->users) {
		check_disk_change(device);
	}
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}


static int enc_ram_release(struct gendisk *disk, fmode_t mode)
{
	struct enc_ram_dev *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;

	if (!dev->users) {
		dev->timer.expires = jiffies + INVALIDATE_DELAY;
		add_timer(&dev->timer);
	}
	spin_unlock(&dev->lock);

	return 0;
}

//Check for a media change
int enc_ram_media_changed(struct gendisk *gd)
{
	struct enc_ram_dev *dev = gd->private_data;

	return dev->media_change;
}


//Revalidate
int enc_ram_revalidate(struct gendisk *gd)
{
	struct enc_ram_dev *dev = gd->private_data;

	if (dev->media_change) {
		dev->media_change = 0;
		memset(dev->data, 0, dev->size);
	}
	return 0;
}

//function to deal with timeout
void enc_ram_invalidate(unsigned long ldev)
{
	struct enc_ram_dev *dev = (struct enc_ram_dev *) ldev;

	spin_lock(&dev->lock);
	if (dev->users || !dev->data) {
		printk(KERN_WARNING "enc_ram: timer check failed\n");
	} else {
		dev->media_change = 1;
	spin_unlock(&dev->lock);
}


//The ioctl() implementation
//Moved to enc_ram_getgeo()
int enc_ram_ioctl (struct block_device *bdev, fmode_t mode,
                 unsigned int cmd, unsigned long arg)
{
		return 0;
}

//New location of geometry detecting code
static int enc_ram_getgeo(struct block_device *device, struct hd_geometry *geo)
{
	struct enc_ram_dev *dev = device->bd_disk->private_data;
	unsigned long size = dev->size * (hardsect_size / KERNEL_SECTOR_SIZE);

	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;
	return 0;
}

//Which functions do which things
static struct block_device_operations enc_ram_ops = {
	.owner = THIS_MODULE,
	.open = enc_ram_open,
	.release = enc_ram_release,
	.media_changed = enc_ram_media_changed,
	.revalidate_disk = enc_ram_revalidate,
	.ioctl = enc_ram_ioctl,
	.getgeo = enc_ram_getgeo
};

//Set up the internal device
static void setup_device(struct enc_ram_dev *dev, int which)
{
	memset(dev, 0, sizeof (struct enc_ram_dev));
	dev->size = nsectors * hardsect_size; 
	dev->data = vmalloc(dev->size); 
	if (dev->data == NULL) {
		printk(KERN_NOTICE "vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);

	//timer used by invalidate function
	init_timer(&dev->timer);
	dev->timer.data = (unsigned long) dev;
	dev->timer.function = enc_ram_invalidate;

	//i/o queueing if not manual
	switch (request_mode) {
	case RM_NOQUEUE:
		dev->queue = blk_alloc_queue(GFP_KERNEL);
		if (dev->queue == NULL)
			goto out_vfree;
		blk_queue_make_request(dev->queue, enc_ram_make_request);
		break;
	case RM_FULL:
		dev->queue = blk_init_queue(enc_ram_full_request, &dev->lock);
		if (dev->queue == NULL)
			goto out_vfree;
		break;
	default:
		printk(KERN_NOTICE
		       "Bad request mode %d, using simple\n", request_mode);
		/* fall into vvv */
	case RM_SIMPLE:
		dev->queue = blk_init_queue(enc_ram_request, &dev->lock);
		if (dev->queue == NULL)
			goto out_vfree;
		break;
	}
	blk_queue_logical_block_size(dev->queue, hardsect_size);
	dev->queue->queuedata = dev;

	//gendisk stuff
	dev->gd = alloc_disk(ENC_RAM_MINORS);
	if (!dev->gd) {
		printk(KERN_NOTICE "alloc_disk failure\n");
		goto out_vfree;
	}
	dev->gd->major = enc_ram_major;
	dev->gd->first_minor = which * ENC_RAM_MINORS;
	dev->gd->fops = &enc_ram_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf(dev->gd->disk_name, 32, "enc_ram%c", which + 'a');
	set_capacity(dev->gd, nsectors * (hardsect_size / KERNEL_SECTOR_SIZE));
	add_disk(dev->gd);
	return;

	   out_vfree:
	if (dev->data)
		vfree(dev->data);
}

static int __init enc_ram_init(void)
{
	int i;

	tfm = crypto_alloc_cipher("aes", 0, 0); 

	//register device
	enc_ram_major = register_blkdev(enc_ram_major, "enc_ram");
	if (enc_ram_major <= 0) {
		printk(KERN_WARNING "enc_ram: unable to get major number\n");
		return -EBUSY;
	}

	//allocate/init the device array
	Devices = kmalloc(ndevices * sizeof (struct enc_ram_dev), GFP_KERNEL);
	if (Devices == NULL)
		goto out_unregister;
	for (i = 0; i < ndevices; i++)
		setup_device(Devices + i, i);

	return 0;
	   out_unregister:
		unregister_blkdev(enc_ram_major, "enc_ram");
		return -ENOMEM;
}

static void enc_ram_exit(void)
{
	int i;

	crypto_free_cipher(tfm);

	for (i = 0; i < ndevices; i++) {
		struct enc_ram_dev *dev = Devices + i;

		del_timer_sync(&dev->timer);
		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if (dev->queue)
			blk_cleanup_queue(dev->queue);
		if (dev->data)
			vfree(dev->data);
	}
	unregister_blkdev(enc_ram_major, "enc_ram");
	kfree(Devices);
}

module_init(enc_ram_init);
module_exit(enc_ram_exit);

