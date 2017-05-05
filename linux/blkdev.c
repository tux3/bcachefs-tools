
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <libaio.h>

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/kthread.h>

#include "tools-util.h"

static io_context_t aio_ctx;

void generic_make_request(struct bio *bio)
{
	struct iovec *iov;
	struct bvec_iter iter;
	struct bio_vec bv;
	ssize_t ret;
	unsigned i;

	if (bio->bi_opf & REQ_PREFLUSH) {
		ret = fdatasync(bio->bi_bdev->bd_fd);
		if (ret) {
			fprintf(stderr, "fsync error: %m\n");
			bio->bi_error = -EIO;
			bio_endio(bio);
			return;
		}
	}

	i = 0;
	bio_for_each_segment(bv, bio, iter)
		i++;

	iov = alloca(sizeof(*iov) * i);

	i = 0;
	bio_for_each_segment(bv, bio, iter)
		iov[i++] = (struct iovec) {
			.iov_base = page_address(bv.bv_page) + bv.bv_offset,
			.iov_len = bv.bv_len,
		};

	struct iocb iocb = {
		.data		= bio,
		.aio_fildes	= bio->bi_opf & REQ_FUA
			? bio->bi_bdev->bd_sync_fd
			: bio->bi_bdev->bd_fd,
	}, *iocbp = &iocb;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		iocb.aio_lio_opcode	= IO_CMD_PREADV;
		iocb.u.v.vec		= iov;
		iocb.u.v.nr		= i;
		iocb.u.v.offset		= bio->bi_iter.bi_sector << 9;

		if (io_submit(aio_ctx, 1, &iocbp) != 1)
			die("io_submit err: %m");
		break;
	case REQ_OP_WRITE:
		iocb.aio_lio_opcode	= IO_CMD_PWRITEV;
		iocb.u.v.vec		= iov;
		iocb.u.v.nr		= i;
		iocb.u.v.offset		= bio->bi_iter.bi_sector << 9;

		if (io_submit(aio_ctx, 1, &iocbp) != 1)
			die("io_submit err: %m");
		break;
	default:
		BUG();
	}
}

static void submit_bio_wait_endio(struct bio *bio)
{
	complete(bio->bi_private);
}

int submit_bio_wait(struct bio *bio)
{
	struct completion done;

	init_completion(&done);
	bio->bi_private = &done;
	bio->bi_end_io = submit_bio_wait_endio;
	bio->bi_opf |= REQ_SYNC;
	submit_bio(bio);
	wait_for_completion(&done);

	return bio->bi_error;
}

int blkdev_issue_discard(struct block_device *bdev,
			 sector_t sector, sector_t nr_sects,
			 gfp_t gfp_mask, unsigned long flags)
{
	return 0;
}

unsigned bdev_logical_block_size(struct block_device *bdev)
{
	struct stat statbuf;
	unsigned blksize;
	int ret;

	ret = fstat(bdev->bd_fd, &statbuf);
	BUG_ON(ret);

	if (!S_ISBLK(statbuf.st_mode))
		return statbuf.st_blksize >> 9;

	ret = ioctl(bdev->bd_fd, BLKPBSZGET, &blksize);
	BUG_ON(ret);

	return blksize >> 9;
}

sector_t get_capacity(struct gendisk *disk)
{
	struct block_device *bdev =
		container_of(disk, struct block_device, __bd_disk);
	struct stat statbuf;
	u64 bytes;
	int ret;

	ret = fstat(bdev->bd_fd, &statbuf);
	BUG_ON(ret);

	if (!S_ISBLK(statbuf.st_mode))
		return statbuf.st_size >> 9;

	ret = ioctl(bdev->bd_fd, BLKGETSIZE64, &bytes);
	BUG_ON(ret);

	return bytes >> 9;
}

void blkdev_put(struct block_device *bdev, fmode_t mode)
{
	fdatasync(bdev->bd_fd);
	close(bdev->bd_sync_fd);
	close(bdev->bd_fd);
	free(bdev);
}

struct block_device *blkdev_get_by_path(const char *path, fmode_t mode,
					void *holder)
{
	struct block_device *bdev;
	int fd, sync_fd, flags = O_DIRECT;

	if ((mode & (FMODE_READ|FMODE_WRITE)) == (FMODE_READ|FMODE_WRITE))
		flags = O_RDWR;
	else if (mode & FMODE_READ)
		flags = O_RDONLY;
	else if (mode & FMODE_WRITE)
		flags = O_WRONLY;

#if 0
	/* using O_EXCL doesn't work with opening twice for an O_SYNC fd: */
	if (mode & FMODE_EXCL)
		flags |= O_EXCL;
#endif

	fd = open(path, flags);
	if (fd < 0)
		return ERR_PTR(-errno);

	sync_fd = open(path, flags|O_SYNC);
	if (sync_fd < 0) {
		assert(0);
		close(fd);
		return ERR_PTR(-errno);
	}

	bdev = malloc(sizeof(*bdev));
	memset(bdev, 0, sizeof(*bdev));

	strncpy(bdev->name, path, sizeof(bdev->name));
	bdev->name[sizeof(bdev->name) - 1] = '\0';

	bdev->bd_fd		= fd;
	bdev->bd_sync_fd	= sync_fd;
	bdev->bd_holder		= holder;
	bdev->bd_disk		= &bdev->__bd_disk;

	return bdev;
}

void bdput(struct block_device *bdev)
{
	BUG();
}

struct block_device *lookup_bdev(const char *path)
{
	return ERR_PTR(-EINVAL);
}

static int aio_completion_thread(void *arg)
{
	struct io_event events[8], *ev;
	int ret;

	while (1) {
		ret = io_getevents(aio_ctx, 1, ARRAY_SIZE(events),
				   events, NULL);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			die("io_getevents() error: %m");

		for (ev = events; ev < events + ret; ev++) {
			struct bio *bio = (struct bio *) ev->data;

			if (ev->res < 0)
				bio->bi_error = ev->res;
			else if (ev->res != bio->bi_iter.bi_size)
				bio->bi_error = -EIO;

			bio_endio(bio);
		}
	}

	return 0;
}

__attribute__((constructor(102)))
static void blkdev_init(void)
{
	struct task_struct *p;

	if (io_setup(256, &aio_ctx))
		die("io_setup() error: %m");

	p = kthread_run(aio_completion_thread, NULL, "aio_completion");
	BUG_ON(IS_ERR(p));
}
