/* 
 * Copyright (c) [2010-2019] zhenfei.mzf@gmail.com rights reserved.
 * 
 * AntiSpy is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
*/
/**
 * device.c - Low level device io functions. Originated from the Linux-NTFS project.
 */


#include <windows.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <winioctl.h>

#include "types.h"
#include "mst.h"
#include "debug.h"
#include "device.h"
#include "logging.h"
#include "misc.h"
#include "win32_io.h"

#pragma warning( disable: 4244 4133 )

//////////////////////////////////////////////////////////////////////////

#if defined(linux) && defined(_IO) && !defined(BLKGETSIZE)
#define BLKGETSIZE	_IO(0x12,96)  /* Get device size in 512-byte blocks. */
#endif
#if defined(linux) && defined(_IOR) && !defined(BLKGETSIZE64)
#define BLKGETSIZE64	_IOR(0x12,114,size_t)	/* Get device size in bytes. */
#endif
#if defined(linux) && !defined(HDIO_GETGEO)
#define HDIO_GETGEO	0x0301	/* Get device geometry. */
#endif
#if defined(linux) && defined(_IO) && !defined(BLKSSZGET)
#	define BLKSSZGET _IO(0x12,104) /* Get device sector size in bytes. */
#endif
#if defined(linux) && defined(_IO) && !defined(BLKBSZSET)
#	define BLKBSZSET _IOW(0x12,113,size_t) /* Set device block size in bytes. */
#endif

//////////////////////////////////////////////////////////////////////////


struct ntfs_device *
ntfs_device_alloc(
	IN const char *name, 
	IN const long state,
	IN struct ntfs_device_operations *dops, 
	IN void *priv_data
	)
/*++

Routine Description:
   分配一个ntfs device结构体,初始化一小部分
    
Arguments:
  name - name of the device (must be present)
  state - initial device state (usually zero)
  dops - ntfs device operations to use with the device (must be present)
  priv_data - pointer to private data (optional)

--*/
{
	struct ntfs_device *dev;

	if (!name) { // 参数合法性检测
		errno = EINVAL;
		return NULL;
	}

	// 分配内存
	dev = (struct ntfs_device *)ntfs_malloc( sizeof(struct ntfs_device) );
	if ( !dev ) { return NULL; }
	
	// 拷贝名字
	if (!(dev->d_name = strdup(name))) 
	{
		int eo = errno;
		free(dev);
		errno = eo;
		return NULL;
	}

	dev->d_ops = dops;
	dev->d_state = state;
	dev->d_private = priv_data;

	return dev;
}


/**
 * ntfs_device_free - free an ntfs device structure
 * @dev:	ntfs device structure to free
 *
 * Free the ntfs device structure @dev.
 *
 * Return 0 on success or -1 on error with errno set to the error code. The
 * following error codes are defined:
 *	EINVAL		Invalid pointer @dev.
 *	EBUSY		Device is still open. Close it before freeing it!
 */
int ntfs_device_free(struct ntfs_device *dev)
{
	if (!dev) {
		errno = EINVAL;
		return -1;
	}
	if (NDevOpen(dev)) {
		errno = EBUSY;
		return -1;
	}
	free(dev->d_name);
	free(dev);
	return 0;
}



s64 
ntfs_pread(
	IN struct ntfs_device *dev, 
	IN const s64 pos,
	IN s64 count, 
	OUT void *b
	)
/*++

Author: sudami [sudami@163.com]
Time  : 2009/03/02 [2:3:2009 - 17:39]

Routine Description:
    
    
Arguments:
  dev - device to read from
  pos - position in device to read from
  count - number of bytes to read
  b - output data buffer
    
--*/
{
	s64 br ;
	struct ntfs_device_operations *dops;

	ntfs_log_trace("Entering for pos 0x%llx, count 0x%llx.\n", pos, count);
	if (!b || count < 0 || pos < 0) { // 参数合法性检测
		errno = EINVAL;
		return -1;
	}

	if (!count) { return 0; }
	dops = dev->d_ops;
	
	// 读取的内容大小总是 512的倍数
	br = dops->pread( dev, b, count, pos );
	return br;
}


/**
 * ntfs_pwrite - positioned write to disk
 * @dev:	device to write to
 * @pos:	position in file descriptor to write to
 * @count:	number of bytes to write
 * @b:		data buffer to write to disk
 *
 * This function will write @count bytes from data buffer @b to the device @dev
 * at position @pos.
 *
 * On success, return the number of successfully written bytes. If this number
 * is lower than @count this means that the write has been interrupted in
 * flight or that an error was encountered during the write so that the write
 * is partial. 0 means nothing was written (also return 0 when @count is 0).
 *
 * On error and nothing has been written, return -1 with errno set
 * appropriately to the return code of either seek, write, or set
 * to EINVAL in case of invalid arguments.
 */
s64 ntfs_pwrite(struct ntfs_device *dev, const s64 pos, s64 count,
		const void *b)
{
	s64 written, total, ret = -1;
	struct ntfs_device_operations *dops;

	ntfs_log_trace("Entering for pos 0x%llx, count 0x%llx.\n", pos, count);
	if (!b || count < 0 || pos < 0) {
		errno = EINVAL;
		goto out;
	}
	if (!count)
		return 0;
// 	if (NDevReadOnly(dev)) {
// 		errno = EROFS;
// 		goto out;
// 	}
	
	dops = dev->d_ops;

	NDevSetDirty(dev);
	for (total = 0; count; count -= written, total += written) 
	{
		written = dops->pwrite(dev, (const char*)b + total, count, pos + total);
		/* If everything ok, continue. */
		if (written > 0)
			continue;
		/*
		 * If nothing written or error return number of bytes written.
		 */
		if (!written || total)
			break;
		/* Nothing written and error, return error status. */
		total = written;
		break;
	}
	ret = total;
out:	
	return ret;
}

/**
 * ntfs_mst_pread - multi sector transfer (mst) positioned read
 * @dev:	device to read from
 * @pos:	position in file descriptor to read from
 * @count:	number of blocks to read
 * @bksize:	size of each block that needs mst deprotecting
 * @b:		output data buffer
 *
 * Multi sector transfer (mst) positioned read. This function will read @count
 * blocks of size @bksize bytes each from device @dev at position @pos into the
 * the data buffer @b.
 *
 * On success, return the number of successfully read blocks. If this number is
 * lower than @count this means that we have reached end of file, that the read
 * was interrupted, or that an error was encountered during the read so that
 * the read is partial. 0 means end of file or nothing was read (also return 0
 * when @count or @bksize are 0).
 *
 * On error and nothing was read, return -1 with errno set appropriately to the
 * return code of either seek, read, or set to EINVAL in case of invalid
 * arguments.
 *
 * NOTE: If an incomplete multi sector transfer has been detected the magic
 * will have been changed to magic_BAAD but no error will be returned. Thus it
 * is possible that we return count blocks as being read but that any number
 * (between zero and count!) of these blocks is actually subject to a multi
 * sector transfer error. This should be detected by the caller by checking for
 * the magic being "BAAD".
 */
s64 ntfs_mst_pread(struct ntfs_device *dev, const s64 pos, s64 count,
		const u32 bksize, void *b)
{
	s64 br, i;

	if (bksize & (bksize - 1) || bksize % NTFS_BLOCK_SIZE) {
		errno = EINVAL;
		return -1;
	}
	/* Do the read. */
	br = ntfs_pread(dev, pos, count * bksize, b);
	if (br < 0)
		return br;
	/*
	 * Apply fixups to successfully read data, disregarding any errors
	 * returned from the MST fixup function. This is because we want to
	 * fixup everything possible and we rely on the fact that the "BAAD"
	 * magic will be detected later on.
	 */
	count = br / bksize;
	for (i = 0; i < count; ++i)
		ntfs_mst_post_read_fixup((NTFS_RECORD*)
				((u8*)b + i * bksize), bksize);
	/* Finally, return the number of complete blocks read. */
	return count;
}

/**
 * ntfs_mst_pwrite - multi sector transfer (mst) positioned write
 * @dev:	device to write to
 * @pos:	position in file descriptor to write to
 * @count:	number of blocks to write
 * @bksize:	size of each block that needs mst protecting
 * @b:		data buffer to write to disk
 *
 * Multi sector transfer (mst) positioned write. This function will write
 * @count blocks of size @bksize bytes each from data buffer @b to the device
 * @dev at position @pos.
 *
 * On success, return the number of successfully written blocks. If this number
 * is lower than @count this means that the write has been interrupted or that
 * an error was encountered during the write so that the write is partial. 0
 * means nothing was written (also return 0 when @count or @bksize are 0).
 *
 * On error and nothing has been written, return -1 with errno set
 * appropriately to the return code of either seek, write, or set
 * to EINVAL in case of invalid arguments.
 *
 * NOTE: We mst protect the data, write it, then mst deprotect it using a quick
 * deprotect algorithm (no checking). This saves us from making a copy before
 * the write and at the same time causes the usn to be incremented in the
 * buffer. This conceptually fits in better with the idea that cached data is
 * always deprotected and protection is performed when the data is actually
 * going to hit the disk and the cache is immediately deprotected again
 * simulating an mst read on the written data. This way cache coherency is
 * achieved.
 */
s64 ntfs_mst_pwrite(struct ntfs_device *dev, const s64 pos, s64 count,
		const u32 bksize, void *b)
{
	s64 written, i;

	if (count < 0 || bksize % NTFS_BLOCK_SIZE) {
		errno = EINVAL;
		return -1;
	}
	if (!count)
		return 0;
	/* Prepare data for writing. */
	for (i = 0; i < count; ++i) {
		int err;

		err = ntfs_mst_pre_write_fixup((NTFS_RECORD*)
				((u8*)b + i * bksize), bksize);
		if (err < 0) {
			/* Abort write at this position. */
			if (!i)
				return err;
			count = i;
			break;
		}
	}
	/* Write the prepared data. */
	written = ntfs_pwrite(dev, pos, count * bksize, b);
	/* Quickly deprotect the data again. */
	for (i = 0; i < count; ++i)
		ntfs_mst_post_write_fixup((NTFS_RECORD*)((u8*)b + i * bksize));
	if (written <= 0)
		return written;
	/* Finally, return the number of complete blocks written. */
	return written / bksize;
}

/**
 * ntfs_cluster_read - read ntfs clusters
 * @vol:	volume to read from
 * @lcn:	starting logical cluster number
 * @count:	number of clusters to read
 * @b:		output data buffer
 *
 * Read @count ntfs clusters starting at logical cluster number @lcn from
 * volume @vol into buffer @b. Return number of clusters read or -1 on error,
 * with errno set to the error code.
 */
s64 ntfs_cluster_read(const ntfs_volume *vol, const s64 lcn, const s64 count,
		void *b)
{
	s64 br;

	if (!vol || lcn < 0 || count < 0) {
		errno = EINVAL;
		return -1;
	}
	if (vol->nr_clusters < lcn + count) {
		errno = ESPIPE;
		ntfs_log_perror("Trying to read outside of volume "
				"(%lld < %lld)", (LONG64)vol->nr_clusters,
			        (LONG64)lcn + count);
		return -1;
	}
	br = ntfs_pread(vol->dev, lcn << vol->cluster_size_bits,
			count << vol->cluster_size_bits, b);
	if (br < 0) {
		ntfs_log_perror("Error reading cluster(s)");
		return br;
	}
	return br >> vol->cluster_size_bits;
}

/**
 * ntfs_cluster_write - write ntfs clusters
 * @vol:	volume to write to
 * @lcn:	starting logical cluster number
 * @count:	number of clusters to write
 * @b:		data buffer to write to disk
 *
 * Write @count ntfs clusters starting at logical cluster number @lcn from
 * buffer @b to volume @vol. Return the number of clusters written or -1 on
 * error, with errno set to the error code.
 */
s64 ntfs_cluster_write(const ntfs_volume *vol, const s64 lcn,
		const s64 count, const void *b)
{
	s64 bw;

	if (!vol || lcn < 0 || count < 0) {
		errno = EINVAL;
		return -1;
	}
	if (vol->nr_clusters < lcn + count) {
		errno = ESPIPE;
		ntfs_log_perror("Trying to write outside of volume "
				"(%lld < %lld)", (LONG64)vol->nr_clusters,
			        (LONG64)lcn + count);
		return -1;
	}
	if (!NVolReadOnly(vol))
		bw = ntfs_pwrite(vol->dev, lcn << vol->cluster_size_bits,
				count << vol->cluster_size_bits, b);
	else
		bw = count << vol->cluster_size_bits;
	if (bw < 0) {
		ntfs_log_perror("Error writing cluster(s)");
		return bw;
	}
	return bw >> vol->cluster_size_bits;
}

/**
 * ntfs_device_offset_valid - test if a device offset is valid
 * @dev:	open device
 * @ofs:	offset to test for validity
 *
 * Test if the offset @ofs is an existing location on the device described
 * by the open device structure @dev.
 *
 * Return 0 if it is valid and -1 if it is not valid.
 */
static int ntfs_device_offset_valid(struct ntfs_device *dev, s64 ofs)
{
	char ch;

	if (dev->d_ops->seek(dev, ofs, SEEK_SET) >= 0 &&
			dev->d_ops->read(dev, &ch, 1) == 1)
		return 0;
	return -1;
}

/**
 * ntfs_device_size_get - return the size of a device in blocks
 * @dev:	open device
 * @block_size:	block size in bytes in which to return the result
 *
 * Return the number of @block_size sized blocks in the device described by the
 * open device @dev.
 *
 * Adapted from e2fsutils-1.19, Copyright (C) 1995 Theodore Ts'o.
 *
 * On error return -1 with errno set to the error code.
 */
s64 ntfs_device_size_get(struct ntfs_device *dev, int block_size)
{
	s64 high, low;

	if (!dev || block_size <= 0 || (block_size - 1) & block_size) {
		errno = EINVAL;
		return -1;
	}
#ifdef BLKGETSIZE64
	{	u64 size;

		if (dev->d_ops->ioctl(dev, BLKGETSIZE64, &size) >= 0) {
			ntfs_log_debug("BLKGETSIZE64 nr bytes = %llu (0x%llx)\n",
					(ULONG64)size,
					(ULONG64)size);
			return (s64)size / block_size;
		}
	}
#endif
#ifdef BLKGETSIZE
	{	unsigned long size;

		if (dev->d_ops->ioctl(dev, BLKGETSIZE, &size) >= 0) {
			ntfs_log_debug("BLKGETSIZE nr 512 byte blocks = %lu (0x%lx)\n",
					size, size);
			return (s64)size * 512 / block_size;
		}
	}
#endif
#ifdef FDGETPRM
	{       struct floppy_struct this_floppy;

		if (dev->d_ops->ioctl(dev, FDGETPRM, &this_floppy) >= 0) {
			ntfs_log_debug("FDGETPRM nr 512 byte blocks = %lu (0x%lx)\n",
					(unsigned long)this_floppy.size,
					(unsigned long)this_floppy.size);
			return (s64)this_floppy.size * 512 / block_size;
		}
	}
#endif
	/*
	 * We couldn't figure it out by using a specialized ioctl,
	 * so do binary search to find the size of the device.
	 */
	low = 0;
	for (high = 1024; !ntfs_device_offset_valid(dev, high); high <<= 1)
		low = high;
	while (low < high - 1) {
		const s64 mid = (low + high) / 2;

		if (!ntfs_device_offset_valid(dev, mid))
			low = mid;
		else
			high = mid;
	}
	dev->d_ops->seek(dev, 0, SEEK_SET);
	return (low + 1) / block_size;
}

/**
 * ntfs_device_partition_start_sector_get - get starting sector of a partition
 * @dev:	open device
 *
 * On success, return the starting sector of the partition @dev in the parent
 * block device of @dev.  On error return -1 with errno set to the error code.
 *
 * The following error codes are defined:
 *	EINVAL		Input parameter error
 *	EOPNOTSUPP	System does not support HDIO_GETGEO ioctl
 *	ENOTTY		@dev is a file or a device not supporting HDIO_GETGEO
 */
s64 ntfs_device_partition_start_sector_get(struct ntfs_device *dev)
{
	if (!dev) {
		errno = EINVAL;
		return -1;
	}
#ifdef HDIO_GETGEO
	{	struct hd_geometry geo;

		if (!dev->d_ops->ioctl(dev, HDIO_GETGEO, &geo)) {
			ntfs_log_debug("HDIO_GETGEO start_sect = %lu (0x%lx)\n",
					geo.start, geo.start);
			return geo.start;
		}
	}
#else
	errno = EOPNOTSUPP;
#endif
	return -1;
}

/**
 * ntfs_device_heads_get - get number of heads of device
 * @dev:		open device
 *
 * On success, return the number of heads on the device @dev.  On error return
 * -1 with errno set to the error code.
 *
 * The following error codes are defined:
 *	EINVAL		Input parameter error
 *	EOPNOTSUPP	System does not support HDIO_GETGEO ioctl
 *	ENOTTY		@dev is a file or a device not supporting HDIO_GETGEO
 */
int ntfs_device_heads_get(struct ntfs_device *dev)
{
	if (!dev) {
		errno = EINVAL;
		return -1;
	}
#ifdef HDIO_GETGEO
	{	struct hd_geometry geo;

		if (!dev->d_ops->ioctl(dev, HDIO_GETGEO, &geo)) {
			ntfs_log_debug("HDIO_GETGEO heads = %u (0x%x)\n",
					(unsigned)geo.heads,
					(unsigned)geo.heads);
			return geo.heads;
		}
	}
#else
	errno = EOPNOTSUPP;
#endif
	return -1;
}

/**
 * ntfs_device_sectors_per_track_get - get number of sectors per track of device
 * @dev:		open device
 *
 * On success, return the number of sectors per track on the device @dev.  On
 * error return -1 with errno set to the error code.
 *
 * The following error codes are defined:
 *	EINVAL		Input parameter error
 *	EOPNOTSUPP	System does not support HDIO_GETGEO ioctl
 *	ENOTTY		@dev is a file or a device not supporting HDIO_GETGEO
 */
int ntfs_device_sectors_per_track_get(struct ntfs_device *dev)
{
	if (!dev) {
		errno = EINVAL;
		return -1;
	}
#ifdef HDIO_GETGEO
	{	struct hd_geometry geo;

		if (!dev->d_ops->ioctl(dev, HDIO_GETGEO, &geo)) {
			ntfs_log_debug("HDIO_GETGEO sectors_per_track = %u (0x%x)\n",
					(unsigned)geo.sectors,
					(unsigned)geo.sectors);
			return geo.sectors;
		}
	}
#else
	errno = EOPNOTSUPP;
#endif
	return -1;
}

/**
 * ntfs_device_sector_size_get - get sector size of a device
 * @dev:	open device
 *
 * On success, return the sector size in bytes of the device @dev.
 * On error return -1 with errno set to the error code.
 *
 * The following error codes are defined:
 *	EINVAL		Input parameter error
 *	EOPNOTSUPP	System does not support BLKSSZGET ioctl
 *	ENOTTY		@dev is a file or a device not supporting BLKSSZGET
 */
int ntfs_device_sector_size_get(struct ntfs_device *dev)
{
	if (!dev) {
		errno = EINVAL;
		return -1;
	}
#ifdef BLKSSZGET
	{
		int sect_size = 0;

		if (!dev->d_ops->ioctl(dev, BLKSSZGET, &sect_size)) {
			ntfs_log_debug("BLKSSZGET sector size = %d bytes\n",
					sect_size);
			return sect_size;
		}
	}
#else
	errno = EOPNOTSUPP;
#endif
	return -1;
}

/**
 * ntfs_device_block_size_set - set block size of a device
 * @dev:	open device
 * @block_size: block size to set @dev to
 *
 * On success, return 0.
 * On error return -1 with errno set to the error code.
 *
 * The following error codes are defined:
 *	EINVAL		Input parameter error
 *	EOPNOTSUPP	System does not support BLKBSZSET ioctl
 *	ENOTTY		@dev is a file or a device not supporting BLKBSZSET
 */
int 
ntfs_device_block_size_set(
	IN struct ntfs_device *dev,
	IN int block_size
	)
{
	if (!dev) { // 参数合法性检测
		errno = EINVAL;
		return -1;
	}
#ifdef BLKBSZSET
	{
		size_t s_block_size = block_size;
		if (!dev->d_ops->ioctl(dev, BLKBSZSET, &s_block_size)) {
			ntfs_log_debug("Used BLKBSZSET to set block size to %d bytes.\n", block_size);
			return 0;
		}
		/* If not a block device, pretend it was successful. */
		if (!NDevBlock(dev))
			return 0;
	}
#else
	/* If not a block device, pretend it was successful. */
	if (!NDevBlock(dev)) { return 0; }
	errno = EOPNOTSUPP;
#endif

	return -1;
}
