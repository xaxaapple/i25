/*
 * ADAPTED FOR THIS PROJECT, NOT ORIGINAL HERE: taken from the user's own copy of
 * akshaynexus/hakios-arm64e-dada (bsd/dev/redwood_image.c, APSL-2.0): reads one file out of a
 * small RAM-resident APFS image that travels with the kernel payload (an mdev, "md0"/"md1"),
 * used to hand the GPU driver its firmware (the phone's own, from its /vendor/firmware -- never
 * redistributed) before any real root is mounted (docs/handoff.md stage 1, "payload builder").
 * This mechanism (mdevlookup/mdevgetkernelrange + an APFS reader) is unchanged from the source
 * project; only the public entry point's name changed, to match handoff_gpu.c's call to it:
 * redwood_image_file() -> handoff_image_file(). The stage-1 payload builder (not yet written;
 * docs/handoff.md) is what must place such a small image and tell this kernel where it is.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <miscfs/apfs/apfs_lib.h>

extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

const uint8_t *handoff_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* ---- APFS ---- */

struct mem_dev {
	const uint8_t *base;
	uint64_t       length;
};

static int
mem_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct mem_dev *m = ctx;

	if (off > m->length || len > m->length - off) {
		return EIO;
	}
	memcpy(buf, m->base + off, len);
	return 0;
}

static LCK_GRP_DECLARE(image_lck_grp, "redwood_image");
static LCK_MTX_DECLARE(image_lock, &image_lck_grp);    /* kept[]: the drivers start beside each other */
static struct {
	int       disk;
	char      path[96];
	uint8_t  *data;
	uint64_t  size;
} kept[64];

static const uint8_t *
apfs_file(int disk, const uint8_t *base, uint64_t length, const char *dir, const char *name, uint64_t *size)
{
	struct mem_dev m = { .base = base, .length = length };
	struct apfs_dev dev = { .ctx = &m, .size = length, .read = mem_read, .read_data = mem_read };
	struct apfs_nx *nx = NULL;
	struct apfs_vol *vol = NULL;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	char path[96];
	uint64_t id = 0, fsize = 0, off = 0;
	uint8_t dt = 0, *data = NULL;
	uint32_t i;
	int error;

	if (length < 4096 || memcmp(base + 32, "NXSB", 4) != 0) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/%s/%s", dir, name);
	lck_mtx_lock(&image_lock);
	for (i = 0; i < sizeof(kept) / sizeof(kept[0]) && kept[i].data != NULL; i++) {
		if (kept[i].disk == disk && strcmp(kept[i].path, path) == 0) {
			*size = kept[i].size;
			lck_mtx_unlock(&image_lock);
			return kept[i].data;
		}
	}
	nx = kalloc_type(struct apfs_nx, Z_WAITOK | Z_ZERO);
	vol = kalloc_type(struct apfs_vol, Z_WAITOK | Z_ZERO);
	if (nx == NULL || vol == NULL) {
		goto out;
	}
	if (apfs_nx_open(nx, &dev) != 0) {
		kfree_type(struct apfs_nx, nx);
		nx = NULL;
		goto out;
	}
	error = ENOENT;
	for (uint32_t v = 0; v < APFS_NX_MAX_FILE_SYSTEMS && error != 0; v++) {
		if (nx->fs_oid[v] != 0 && apfs_vol_open(nx, v, vol) == 0) {
			error = apfs_path_lookup(vol, path, &id, &dt);
		}
	}
	if (error == 0) {
		error = apfs_inode_get(vol, id, &ino);
	}
	if (error == 0 && (ino.mode & APFS_S_IFMT) != APFS_S_IFREG) {
		error = EISDIR;
	}
	if (error == 0) {
		error = apfs_inode_size(vol, &ino, &fsize);
	}
	if (error == 0 && (fsize == 0 || fsize > 256 * 1024 * 1024)) {
		error = EFBIG;
	}
	if (error == 0) {
		uint64_t addr = 0;

		/* in one piece as it is: where it lies, no copy */
		if (apfs_file_in_place(vol, &ino, &addr) == 0 && addr <= length / nx->block_size &&
		    fsize <= length - addr * nx->block_size) {
			apfs_nx_close(nx);
			*size = fsize;
			data = __DECONST(uint8_t *, base) + addr * nx->block_size;
			goto out;
		}
		if (i == sizeof(kept) / sizeof(kept[0])) {
			printf("redwood_image: %s of /dev/md%d: kept %lu files already, no room for a copy of another\n", path, disk,
			    (unsigned long)i);
			error = ENOSPC;
		}
	}
	if (error == 0) {
		data = kalloc_data((vm_size_t)fsize, Z_WAITOK);
		error = data == NULL ? ENOMEM : 0;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	while (error == 0 && off < fsize) {
		size_t got = 0;

		error = apfs_read_file(vol, &ino, &cf, off, data + off, (size_t)(fsize - off), &got);
		if (error == 0 && got == 0) {
			error = EIO;
		}
		off += got;
	}
	apfs_cfile_fini(&cf);
	apfs_nx_close(nx);
	if (error == 0) {
		kept[i].disk = disk;
		strlcpy(kept[i].path, path, sizeof(kept[i].path));
		kept[i].size = fsize;
		kept[i].data = data;            /* (last: a reader of the table sees all of it or none) */
		*size = fsize;
	} else if (data != NULL) {
		kfree_data(data, (vm_size_t)fsize);
		data = NULL;
	}
out:
	lck_mtx_unlock(&image_lock);
	if (vol != NULL) {
		kfree_type(struct apfs_vol, vol);
	}
	if (nx != NULL) {
		kfree_type(struct apfs_nx, nx);
	}
	return data;
}

const uint8_t *
handoff_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	for (int disk = 1; disk >= 0; disk--) {
		vm_offset_t base = 0;
		uint64_t length = 0;
		const uint8_t *data;
		dev_t dev = mdevlookup(disk);

		if (dev == (dev_t)-1 || mdevgetkernelrange(dev, &base, &length) != 0) {
			continue;
		}
		data = apfs_file(disk, (const uint8_t *)base, length, dir, name, size);
		if (data != NULL) {
			if (disk_out != NULL) {
				*disk_out = disk;
			}
			return data;
		}
	}
	return NULL;
}
