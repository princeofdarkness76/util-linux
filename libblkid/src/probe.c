/*
 * Low-level libblkid probing API
 *
 * Copyright (C) 2008-2009 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

/**
 * SECTION: lowprobe
 * @title: Low-level probing
 * @short_description: low-level prober initialization
 *
 * The low-level probing routines always and directly read information from
 * the selected (see blkid_probe_set_device()) device.
 *
 * The probing routines are grouped together into separate chains. Currently,
 * the library provides superblocks, partitions and topology chains.
 *
 * The probing routines is possible to filter (enable/disable) by type (e.g.
 * fstype "vfat" or partype "gpt") or by usage flags (e.g. BLKID_USAGE_RAID).
 * These filters are per-chain. Note that always when you touch the chain
 * filter the current probing position is reset and probing starts from
 * scratch.  It means that the chain filter should not be modified during
 * probing, for example in loop where you call blkid_do_probe().
 *
 * For more details see the chain specific documentation.
 *
 * The low-level API provides two ways how access to probing results.
 *
 *   1. The NAME=value (tag) interface. This interface is older and returns all data
 *      as strings. This interface is generic for all chains.
 *
 *   2. The binary interfaces. These interfaces return data in the native formats.
 *      The interface is always specific to the probing chain.
 *
 *  Note that the previous probing result (binary or NAME=value) is always
 *  zeroized when a chain probing function is called. For example:
 *
 * <informalexample>
 *   <programlisting>
 *     blkid_probe_enable_partitions(pr, TRUE);
 *     blkid_probe_enable_superblocks(pr, FALSE);
 *
 *     blkid_do_safeprobe(pr);
 *   </programlisting>
 * </informalexample>
 *
 * overwrites the previous probing result for the partitions chain, the superblocks
 * result is not modified.
 */

/**
 * SECTION: lowprobe-tags
 * @title: Low-level tags
 * @short_description: generic NAME=value interface.
 *
 * The probing routines inside the chain are mutually exclusive by default --
 * only few probing routines are marked as "tolerant". The "tolerant" probing
 * routines are used for filesystem which can share the same device with any
 * other filesystem. The blkid_do_safeprobe() checks for the "tolerant" flag.
 *
 * The SUPERBLOCKS chain is enabled by default. The all others chains is
 * necessary to enable by blkid_probe_enable_'CHAINNAME'(). See chains specific
 * documentation.
 *
 * The blkid_do_probe() function returns a result from only one probing
 * routine, and the next call from the next probing routine. It means you need
 * to call the function in loop, for example:
 *
 * <informalexample>
 *   <programlisting>
 *	while((blkid_do_probe(pr) == 0)
 *		... use result ...
 *   </programlisting>
 * </informalexample>
 *
 * The blkid_do_safeprobe() is the same as blkid_do_probe(), but returns only
 * first probing result for every enabled chain. This function checks for
 * ambivalent results (e.g. more "intolerant" filesystems superblocks on the
 * device).
 *
 * The probing result is set of NAME=value pairs (the NAME is always unique).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#ifdef HAVE_LINUX_CDROM_H
#include <linux/cdrom.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <inttypes.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/mman.h>

#ifdef HAVE_LIBUUID
# include <uuid.h>
#endif

#include "blkidP.h"
#include "all-io.h"
#include "sysfs.h"
#include "strutils.h"
#include "list.h"

/* chains */
extern const struct blkid_chaindrv superblocks_drv;
extern const struct blkid_chaindrv topology_drv;
extern const struct blkid_chaindrv partitions_drv;

/*
 * All supported chains
 */
static const struct blkid_chaindrv *chains_drvs[] = {
	[BLKID_CHAIN_SUBLKS] = &superblocks_drv,
	[BLKID_CHAIN_TOPLGY] = &topology_drv,
	[BLKID_CHAIN_PARTS] = &partitions_drv
};

static struct blkid_prval *blkid_probe_new_value(void);
static void blkid_probe_reset_values(blkid_probe pr);
static void blkid_probe_reset_buffer(blkid_probe pr);

blkid_probe __blkid_new_probe(struct blkid_config *conf)
{
	int i;
	blkid_probe pr;

	blkid_init_debug(0);
	pr = calloc(1, sizeof(struct blkid_struct_probe));
	if (!pr)
		return NULL;

	DBG(LOWPROBE, ul_debug("allocate a new probe %p", pr));

	/* initialize chains */
	for (i = 0; i < BLKID_NCHAINS; i++) {
		pr->chains[i].driver = chains_drvs[i];
		pr->chains[i].flags = chains_drvs[i]->dflt_flags;
		pr->chains[i].enabled = chains_drvs[i]->dflt_enabled;
	}
	INIT_LIST_HEAD(&pr->buffers);
	INIT_LIST_HEAD(&pr->values);

	if (!conf)
		conf = blkid_read_config();
	blkid_probe_set_config(pr, conf);

	return pr;
}

/**
 * blkid_new_probe:
 *
 * Returns: a pointer to the newly allocated probe struct or NULL in case of error.
 */
blkid_probe blkid_new_probe(void)
{
	return __blkid_new_probe(NULL);
}

void blkid_probe_set_config(blkid_probe pr, struct blkid_config *conf)
{
	assert(pr);

	if (conf)
		blkid_ref_config(conf);

	blkid_unref_config(pr->conf);
	pr->conf = conf;
	if (!conf || !conf->probeoff)
		return;

	__blkid_probe_filter_types(pr, BLKID_CHAIN_SUBLKS, BLKID_FLTR_NOTIN, conf->probeoff);
}

struct blkid_config *blkid_probe_get_config(blkid_probe pr)
{
	assert(pr);

	if (!pr->conf)
		pr->conf = blkid_read_config();

	return pr->conf;
}

/*
 * Clone @parent, the new clone shares all, but except:
 *
 *	- probing result
 *	- bufferes if another device (or offset) is set to the prober
 */
blkid_probe blkid_clone_probe(blkid_probe parent)
{
	blkid_probe pr;

	if (!parent)
		return NULL;

	DBG(LOWPROBE, ul_debug("allocate a probe clone"));

	pr = blkid_new_probe();
	if (!pr)
		return NULL;

	pr->fd = parent->fd;
	pr->off = parent->off;
	pr->size = parent->size;
	pr->devno = parent->devno;
	pr->disk_devno = parent->disk_devno;
	pr->blkssz = parent->blkssz;
	pr->flags = parent->flags;
	pr->parent = parent;

	pr->flags &= ~BLKID_FL_PRIVATE_FD;

	blkid_probe_set_config(pr, parent->conf);

	return pr;
}



/**
 * blkid_new_probe_from_filename:
 * @filename: device or regular file
 *
 * This function is same as call open(filename), blkid_new_probe() and
 * blkid_probe_set_device(pr, fd, 0, 0).
 *
 * The @filename is closed by blkid_free_probe() or by the
 * blkid_probe_set_device() call.
 *
 * Returns: a pointer to the newly allocated probe struct or NULL in case of
 * error.
 */
blkid_probe blkid_new_probe_from_filename(const char *filename)
{
	int fd = -1;
	blkid_probe pr = NULL;

	if (!filename)
		return NULL;

	fd = open(filename, O_RDONLY|O_CLOEXEC);
	if (fd < 0)
		return NULL;

	pr = blkid_new_probe();
	if (!pr)
		goto err;

	if (blkid_probe_set_device(pr, fd, 0, 0))
		goto err;

	pr->flags |= BLKID_FL_PRIVATE_FD;
	return pr;
err:
	if (fd >= 0)
		close(fd);
	blkid_free_probe(pr);
	return NULL;
}

/**
 * blkid_free_probe:
 * @pr: probe
 *
 * Deallocates the probe struct, buffers and all allocated
 * data that are associated with this probing control struct.
 */
void blkid_free_probe(blkid_probe pr)
{
	int i;

	if (!pr)
		return;

	for (i = 0; i < BLKID_NCHAINS; i++) {
		struct blkid_chain *ch = &pr->chains[i];

		if (ch->driver->free_data)
			ch->driver->free_data(pr, ch->data);
		free(ch->fltr);
	}

	if ((pr->flags & BLKID_FL_PRIVATE_FD) && pr->fd >= 0)
		close(pr->fd);
	blkid_probe_reset_buffer(pr);
	blkid_probe_reset_values(pr);
	blkid_free_probe(pr->disk_probe);
	blkid_unref_config(pr->conf);

	DBG(LOWPROBE, ul_debug("free probe %p", pr));
	free(pr);
}

void blkid_probe_free_value(struct blkid_prval *v)
{
	if (!v)
		return;

	list_del(&v->prvals);
	free(v->data);

	DBG(LOWPROBE, ul_debug(" free value %s", v->name));
	free(v);
}

/*
 * Removes chain values from probing result.
 */
void blkid_probe_chain_reset_values(blkid_probe pr, struct blkid_chain *chn)
{

	struct list_head *p, *pnext;

	if (!pr || list_empty(&pr->values))
		return;

	DBG(LOWPROBE, ul_debug("reseting %s values", chn->driver->name));

	list_for_each_safe(p, pnext, &pr->values) {
		struct blkid_prval *v = list_entry(p,
						struct blkid_prval, prvals);

		if (v->chain == chn)
			blkid_probe_free_value(v);
	}
}

static void blkid_probe_chain_reset_position(struct blkid_chain *chn)
{
	if (chn)
		chn->idx = -1;
}

/*
static struct blkid_prval *blkid_probe_copy_value(struct blkid_prval *src)
{
	struct blkid_prval *dest = blkid_probe_new_value();

	if (!dest)
		return NULL;

	memcpy(dest, src, sizeof(struct blkid_prval));

	dest->data = malloc(src->len);
	if (!dest->data)
		return NULL;

	memcpy(dest->data, src->data, src->len);

	INIT_LIST_HEAD(&dest->prvals);
	return dest;
}
*/

/*
 * Move chain values from probing result to @vals
 */
int blkid_probe_chain_save_values(blkid_probe pr, struct blkid_chain *chn,
				struct list_head *vals)
{
	struct list_head *p, *pnext;
	struct blkid_prval *v;

	DBG(LOWPROBE, ul_debug("saving %s values", chn->driver->name));

	list_for_each_safe(p, pnext, &pr->values) {

		v = list_entry(p, struct blkid_prval, prvals);
		if (v->chain != chn)
			continue;

		list_del(&v->prvals);
		INIT_LIST_HEAD(&v->prvals);

		list_add_tail(&v->prvals, vals);
	}
	return 0;
}

/*
 * Appends values from @vals to the probing result
 */
void blkid_probe_append_values_list(blkid_probe pr, struct list_head *vals)
{
	DBG(LOWPROBE, ul_debug("appending values"));

	list_splice(vals, &pr->values);
	INIT_LIST_HEAD(vals);
}


void blkid_probe_free_values_list(struct list_head *vals)
{
	if (!vals)
		return;

	DBG(LOWPROBE, ul_debug("freeing values list"));

	while (!list_empty(vals)) {
		struct blkid_prval *v = list_entry(vals->next, struct blkid_prval, prvals);
		blkid_probe_free_value(v);
	}
}

struct blkid_chain *blkid_probe_get_chain(blkid_probe pr)
{
	return pr->cur_chain;
}

static const char *blkid_probe_get_probername(blkid_probe pr)
{
	struct blkid_chain *chn = blkid_probe_get_chain(pr);

	if (chn && chn->idx >= 0 && chn->idx < chn->driver->nidinfos)
		return chn->driver->idinfos[chn->idx]->name;

	return NULL;
}

void *blkid_probe_get_binary_data(blkid_probe pr, struct blkid_chain *chn)
{
	int rc, org_prob_flags;
	struct blkid_chain *org_chn;

	if (!pr || !chn)
		return NULL;

	/* save the current setting -- the binary API has to be completely
	 * independent on the current probing status
	 */
	org_chn = pr->cur_chain;
	org_prob_flags = pr->prob_flags;

	pr->cur_chain = chn;
	pr->prob_flags = 0;
	chn->binary = TRUE;
	blkid_probe_chain_reset_position(chn);

	rc = chn->driver->probe(pr, chn);

	chn->binary = FALSE;
	blkid_probe_chain_reset_position(chn);

	/* restore the original setting
	 */
	pr->cur_chain = org_chn;
	pr->prob_flags = org_prob_flags;

	if (rc != 0)
		return NULL;

	DBG(LOWPROBE, ul_debug("returning %s binary data", chn->driver->name));
	return chn->data;
}


/**
 * blkid_reset_probe:
 * @pr: probe
 *
 * Zeroize probing results and resets the current probing (this has impact to
 * blkid_do_probe() only). This function does not touch probing filters and
 * keeps assigned device.
 */
void blkid_reset_probe(blkid_probe pr)
{
	int i;

	if (!pr)
		return;

	blkid_probe_reset_values(pr);
	blkid_probe_set_wiper(pr, 0, 0);

	pr->cur_chain = NULL;

	for (i = 0; i < BLKID_NCHAINS; i++)
		blkid_probe_chain_reset_position(&pr->chains[i]);
}

/***
static int blkid_probe_dump_filter(blkid_probe pr, int chain)
{
	struct blkid_chain *chn;
	int i;

	if (!pr || chain < 0 || chain >= BLKID_NCHAINS)
		return -1;

	chn = &pr->chains[chain];

	if (!chn->fltr)
		return -1;

	for (i = 0; i < chn->driver->nidinfos; i++) {
		const struct blkid_idinfo *id = chn->driver->idinfos[i];

		DBG(LOWPROBE, ul_debug("%d: %s: %s",
			i,
			id->name,
			blkid_bmp_get_item(chn->fltr, i)
				? "disabled" : "enabled <--"));
	}
	return 0;
}
***/

/*
 * Returns properly initialized chain filter
 */
unsigned long *blkid_probe_get_filter(blkid_probe pr, int chain, int create)
{
	struct blkid_chain *chn;

	if (!pr || chain < 0 || chain >= BLKID_NCHAINS)
		return NULL;

	chn = &pr->chains[chain];

	/* always when you touch the chain filter all indexes are reset and
	 * probing starts from scratch
	 */
	blkid_probe_chain_reset_position(chn);
	pr->cur_chain = NULL;

	if (!chn->driver->has_fltr || (!chn->fltr && !create))
		return NULL;

	if (!chn->fltr)
		chn->fltr = calloc(1, blkid_bmp_nbytes(chn->driver->nidinfos));
	else
		memset(chn->fltr, 0, blkid_bmp_nbytes(chn->driver->nidinfos));

	/* blkid_probe_dump_filter(pr, chain); */
	return chn->fltr;
}

/*
 * Generic private functions for filter setting
 */
int __blkid_probe_invert_filter(blkid_probe pr, int chain)
{
	size_t i;
	struct blkid_chain *chn;

	if (!pr)
		return -1;

	chn = &pr->chains[chain];

	if (!chn->driver->has_fltr || !chn->fltr)
		return -1;

	for (i = 0; i < blkid_bmp_nwords(chn->driver->nidinfos); i++)
		chn->fltr[i] = ~chn->fltr[i];

	DBG(LOWPROBE, ul_debug("probing filter inverted"));
	/* blkid_probe_dump_filter(pr, chain); */
	return 0;
}

int __blkid_probe_reset_filter(blkid_probe pr, int chain)
{
	return blkid_probe_get_filter(pr, chain, FALSE) ? 0 : -1;
}

int __blkid_probe_filter_types(blkid_probe pr, int chain, int flag, char *names[])
{
	unsigned long *fltr;
	struct blkid_chain *chn;
	size_t i;

	fltr = blkid_probe_get_filter(pr, chain, TRUE);
	if (!fltr)
		return -1;

	chn = &pr->chains[chain];

	for (i = 0; i < chn->driver->nidinfos; i++) {
		int has = 0;
		const struct blkid_idinfo *id = chn->driver->idinfos[i];
		char **n;

		for (n = names; *n; n++) {
			if (!strcmp(id->name, *n)) {
				has = 1;
				break;
			}
		}
		if (flag & BLKID_FLTR_ONLYIN) {
		       if (!has)
				blkid_bmp_set_item(fltr, i);
		} else if (flag & BLKID_FLTR_NOTIN) {
			if (has)
				blkid_bmp_set_item(fltr, i);
		}
	}

	DBG(LOWPROBE, ul_debug("%s: a new probing type-filter initialized",
		chn->driver->name));
	/* blkid_probe_dump_filter(pr, chain); */
	return 0;
}

/* align to mmap granularity */
#define PROBE_ALIGN_OFF(p, o)	((o) & ~((p)->mmap_granularity - 1ULL))
/* default buffer sizes */
#define PROBE_MMAP_BEGINSIZ	(1024ULL * 1024ULL * 2ULL)	/* begin of the device */
#define PROBE_MMAP_ENDSIZ	(1024ULL * 1024ULL * 2ULL)	/* end of the device */
#define PROBE_MMAP_MIDSIZ	(1024ULL * 1024ULL)		/* middle of the device */

#define probe_is_mmap_wanted(p)		(!S_ISCHR((p)->mode))

static struct blkid_bufinfo *mmap_buffer(blkid_probe pr, uint64_t real_off, uint64_t len)
{
	uint64_t map_len;
	uint64_t map_off = 0;
	struct blkid_bufinfo *bf = NULL;

	/*
	 * libblkid heavily reads begin and end of the device, so it seems
	 * better to mmap ~2MiB from the begin and end of the device to reduces
	 * number of syscalls and necessary buffers. For random accees
	 * somewhere in the middle of the device we use 1MiB buffers.
	 */
	if (!pr->mmap_granularity)
		pr->mmap_granularity = getpagesize();

	/* begin of the device */
	if (real_off == 0 || real_off + len < PROBE_MMAP_BEGINSIZ) {
		DBG(BUFFER, ul_debug("\tmapping begin of the device (max size: %ju)", pr->size));
		map_off = 0;
		map_len = PROBE_MMAP_BEGINSIZ > pr->size ? pr->size : PROBE_MMAP_BEGINSIZ;


	/* end of the device */
	} else if (real_off > pr->off + pr->size - PROBE_MMAP_ENDSIZ) {
		DBG(BUFFER, ul_debug("\tmapping end of the device (probing area: "
					"off=%ju, size=%ju)", pr->off, pr->size));

		map_off = PROBE_ALIGN_OFF(pr, pr->off + pr->size - PROBE_MMAP_ENDSIZ);
		map_len = pr->off + pr->size - map_off;

	/* middle of the device */
	} else {
		uint64_t minlen;

		map_off = PROBE_ALIGN_OFF(pr, real_off);
		minlen = real_off + len - map_off;

		map_len = minlen > PROBE_MMAP_MIDSIZ ? minlen : PROBE_MMAP_MIDSIZ;

		if (map_off + map_len > pr->off + pr->size)
			map_len = pr->size - map_off;
	}

	assert(map_off <= real_off);
	assert(map_off + map_len >= real_off + len);

	/* allocate buffer handler */
	bf = malloc(sizeof(*bf));
	if (!bf) {
		errno = ENOMEM;
		return NULL;
	}

	/* mmap into memmory */
	bf->data = mmap(NULL, map_len, PROT_READ, MAP_SHARED, pr->fd, map_off);
	if (bf->data == MAP_FAILED) {
		DBG(BUFFER, ul_debug("\tmmap failed: %m"));
		free(bf);
		return NULL;
	}

	bf->off = map_off;
	bf->len = map_len;
	INIT_LIST_HEAD(&bf->bufs);

	DBG(BUFFER, ul_debug("\tmmap  %p: off=%ju, len=%ju (%ju pages)",
				bf->data, map_off, map_len, map_len / pr->mmap_granularity));
	return bf;
}

static struct blkid_bufinfo *read_buffer(blkid_probe pr, uint64_t real_off, uint64_t len)
{
	ssize_t ret;
	struct blkid_bufinfo *bf = NULL;

	if (blkid_llseek(pr->fd, real_off, SEEK_SET) < 0) {
		errno = 0;
		return NULL;
	}

	/* someone trying to overflow some buffers? */
	if (len > ULONG_MAX - sizeof(struct blkid_bufinfo)) {
		errno = ENOMEM;
		return NULL;
	}

	/* allocate info and space for data by one malloc call */
	bf = calloc(1, sizeof(struct blkid_bufinfo) + len);
	if (!bf) {
		errno = ENOMEM;
		return NULL;
	}

	bf->data = ((unsigned char *) bf) + sizeof(struct blkid_bufinfo);
	bf->len = len;
	bf->off = real_off;
	INIT_LIST_HEAD(&bf->bufs);

	DBG(LOWPROBE, ul_debug("\tread %p: off=%ju len=%ju", bf->data, real_off, len));

	ret = read(pr->fd, bf->data, len);
	if (ret != (ssize_t) len) {
		DBG(LOWPROBE, ul_debug("\tread failed: %m"));
		free(bf);
		if (ret >= 0)
			errno = 0;
		return NULL;
	}

	return bf;
}

/*
 * Note that @off is offset within probing area, the probing area is defined by
 * pr->off and pr->size.
 */
unsigned char *blkid_probe_get_buffer(blkid_probe pr, uint64_t off, uint64_t len)
{
	struct list_head *p;
	struct blkid_bufinfo *bf = NULL;
	uint64_t real_off = pr->off + off;

	/*
	DBG(BUFFER, ul_debug("\t>>>> off=%ju, real-off=%ju (probe <%ju..%ju>, len=%ju",
				off, real_off, pr->off, pr->off + pr->size, len));
	*/

	if (pr->size == 0) {
		errno = EINVAL;
		return NULL;
	}

	if (len == 0 || pr->off + pr->size < real_off + len) {
		DBG(BUFFER, ul_debug("\t  ignore: request out of probing area"));
		errno = 0;
		return NULL;
	}

	if (pr->parent &&
	    pr->parent->devno == pr->devno &&
	    pr->parent->off <= pr->off &&
	    pr->parent->off + pr->parent->size >= pr->off + pr->size) {
		/*
		 * This is a cloned prober and points to the same area as
		 * parent. Let's use parent's buffers.
		 *
		 * Note that pr->off (and pr->parent->off) is always from the
		 * begin of the device.
		 */
		return blkid_probe_get_buffer(pr->parent,
				pr->off + off - pr->parent->off, len);
	}

	/* try buffers we already have in memmory */
	list_for_each(p, &pr->buffers) {
		struct blkid_bufinfo *x =
				list_entry(p, struct blkid_bufinfo, bufs);

		if (real_off >= x->off && real_off + len <= x->off + x->len) {
			DBG(BUFFER, ul_debug("\treuse %p: off=%ju len=%ju (for off=%ju len=%ju)",
						x->data, x->off, x->len, real_off, len));
			bf = x;
			break;
		}
	}

	/* not found; read from disk */
	if (!bf) {
		if (probe_is_mmap_wanted(pr))
			bf = mmap_buffer(pr, real_off, len);
		else
			bf = read_buffer(pr, real_off, len);
		if (!bf)
			return NULL;

		list_add_tail(&bf->bufs, &pr->buffers);
	}

	assert(bf->off <= real_off);
	assert(bf->off + bf->len >= real_off + len);

	errno = 0;
	return real_off ? bf->data + (real_off - bf->off) : bf->data;
}

static void blkid_probe_reset_buffer(blkid_probe pr)
{
	uint64_t ct = 0, len = 0;

	if (!pr || list_empty(&pr->buffers))
		return;

	DBG(BUFFER, ul_debug("reseting probing buffers pr=%p", pr));

	while (!list_empty(&pr->buffers)) {
		struct blkid_bufinfo *bf = list_entry(pr->buffers.next,
						struct blkid_bufinfo, bufs);
		ct++;
		len += bf->len;
		list_del(&bf->bufs);

		DBG(BUFFER, ul_debug(" remove buffer: %p [off=%ju, len=%ju]", bf->data, bf->off, bf->len));

		if (probe_is_mmap_wanted(pr))
			munmap(bf->data, bf->len);
		free(bf);
	}

	DBG(LOWPROBE, ul_debug(" buffers summary: %ju bytes by %ju read/mmap() calls",
			len, ct));

	INIT_LIST_HEAD(&pr->buffers);
}

static void blkid_probe_reset_values(blkid_probe pr)
{
	if (!pr || list_empty(&pr->values))
		return;

	DBG(LOWPROBE, ul_debug("resetting results pr=%p", pr));

	while (!list_empty(&pr->values)) {
		struct blkid_prval *v = list_entry(pr->values.next,
						struct blkid_prval, prvals);
		blkid_probe_free_value(v);
	}

	INIT_LIST_HEAD(&pr->values);
}

/*
 * Small devices need a special care.
 */
int blkid_probe_is_tiny(blkid_probe pr)
{
	return pr && (pr->flags & BLKID_FL_TINY_DEV);
}

/*
 * CDROMs may fail when probed for RAID (last sector problem)
 */
int blkid_probe_is_cdrom(blkid_probe pr)
{
	return pr && (pr->flags & BLKID_FL_CDROM_DEV);
}

/**
 * blkid_probe_set_device:
 * @pr: probe
 * @fd: device file descriptor
 * @off: begin of probing area
 * @size: size of probing area (zero means whole device/file)
 *
 * Assigns the device to probe control struct, resets internal buffers and
 * resets the current probing.
 *
 * Returns: -1 in case of failure, or 0 on success.
 */
int blkid_probe_set_device(blkid_probe pr, int fd,
		blkid_loff_t off, blkid_loff_t size)
{
	struct stat sb;
	uint64_t devsiz = 0;

	if (!pr)
		return -1;

	blkid_reset_probe(pr);
	blkid_probe_reset_buffer(pr);

	if ((pr->flags & BLKID_FL_PRIVATE_FD) && pr->fd >= 0)
		close(pr->fd);

	pr->flags &= ~BLKID_FL_PRIVATE_FD;
	pr->flags &= ~BLKID_FL_TINY_DEV;
	pr->flags &= ~BLKID_FL_CDROM_DEV;
	pr->prob_flags = 0;
	pr->fd = fd;
	pr->off = (uint64_t) off;
	pr->size = 0;
	pr->devno = 0;
	pr->disk_devno = 0;
	pr->mode = 0;
	pr->blkssz = 0;
	pr->wipe_off = 0;
	pr->wipe_size = 0;
	pr->wipe_chain = NULL;

#if defined(POSIX_FADV_RANDOM) && defined(HAVE_POSIX_FADVISE)
	/* Disable read-ahead */
	posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
#endif
	if (fstat(fd, &sb))
		goto err;

	if (!S_ISBLK(sb.st_mode) && !S_ISCHR(sb.st_mode) && !S_ISREG(sb.st_mode)) {
		errno = EINVAL;
		goto err;
	}

	pr->mode = sb.st_mode;
	if (S_ISBLK(sb.st_mode) || S_ISCHR(sb.st_mode))
		pr->devno = sb.st_rdev;

	if (S_ISBLK(sb.st_mode)) {
		if (blkdev_get_size(fd, (unsigned long long *) &devsiz)) {
			DBG(LOWPROBE, ul_debug("failed to get device size"));
			goto err;
		}
	} else if (S_ISCHR(sb.st_mode))
		devsiz = 1;		/* UBI devices are char... */
	else if (S_ISREG(sb.st_mode))
		devsiz = sb.st_size;	/* regular file */

	pr->size = size ? size : devsiz;

	if (off && size == 0)
		/* only offset without size specified */
		pr->size -= (uint64_t) off;

	if (pr->off + pr->size > devsiz) {
		DBG(LOWPROBE, ul_debug("area specified by offset and size is bigger than device"));
		errno = EINVAL;
		goto err;
	}

	if (pr->size <= 1440 * 1024 && !S_ISCHR(sb.st_mode))
		pr->flags |= BLKID_FL_TINY_DEV;

	if (S_ISBLK(sb.st_mode) && sysfs_devno_is_lvm_private(sb.st_rdev)) {
		DBG(LOWPROBE, ul_debug("ignore private LVM device"));
		pr->flags |= BLKID_FL_NOSCAN_DEV;
	}

#ifdef CDROM_GET_CAPABILITY
	else if (S_ISBLK(sb.st_mode) &&
	    !blkid_probe_is_tiny(pr) &&
	    blkid_probe_is_wholedisk(pr) &&
	    ioctl(fd, CDROM_GET_CAPABILITY, NULL) >= 0)
		pr->flags |= BLKID_FL_CDROM_DEV;
#endif

	DBG(LOWPROBE, ul_debug("ready for low-probing, offset=%ju, size=%ju",
				pr->off, pr->size));
	DBG(LOWPROBE, ul_debug("whole-disk: %s, regfile: %s",
		blkid_probe_is_wholedisk(pr) ?"YES" : "NO",
		S_ISREG(pr->mode) ? "YES" : "NO"));

	return 0;
err:
	DBG(LOWPROBE, ul_debug("failed to prepare a device for low-probing"));
	return -1;

}

int blkid_probe_get_dimension(blkid_probe pr, uint64_t *off, uint64_t *size)
{
	if (!pr)
		return -1;

	*off = pr->off;
	*size = pr->size;
	return 0;
}

int blkid_probe_set_dimension(blkid_probe pr, uint64_t off, uint64_t size)
{
	if (!pr)
		return -1;

	DBG(LOWPROBE, ul_debug(
		"changing probing area pr=%p: size=%ju, off=%ju "
		"-to-> size=%ju, off=%ju",
		pr, pr->size, pr->off, size, off));

	pr->off = off;
	pr->size = size;
	pr->flags &= ~BLKID_FL_TINY_DEV;

	if (pr->size <= 1440ULL * 1024ULL && !S_ISCHR(pr->mode))
		pr->flags |= BLKID_FL_TINY_DEV;

	blkid_probe_reset_buffer(pr);

	return 0;
}

/*
 * Check for matching magic value.
 * Returns BLKID_PROBE_OK if found, BLKID_PROBE_NONE if not found
 * or no magic present, or negative value on error.
 */
int blkid_probe_get_idmag(blkid_probe pr, const struct blkid_idinfo *id,
			uint64_t *offset, const struct blkid_idmag **res)
{
	const struct blkid_idmag *mag = NULL;
	uint64_t off = 0;

	if (id)
		mag = &id->magics[0];
	if (res)
		*res = NULL;

	/* try to detect by magic string */
	while(mag && mag->magic) {
		unsigned char *buf;

		off = (mag->kboff + (mag->sboff >> 10)) << 10;
		buf = blkid_probe_get_buffer(pr, off, 1024);

		if (!buf && errno)
			return -errno;

		if (buf && !memcmp(mag->magic,
				buf + (mag->sboff & 0x3ff), mag->len)) {

			DBG(LOWPROBE, ul_debug("\tmagic sboff=%u, kboff=%ld",
				mag->sboff, mag->kboff));
			if (offset)
				*offset = off + (mag->sboff & 0x3ff);
			if (res)
				*res = mag;
			return BLKID_PROBE_OK;
		}
		mag++;
	}

	if (id && id->magics[0].magic)
		/* magic string(s) defined, but not found */
		return BLKID_PROBE_NONE;

	return BLKID_PROBE_OK;
}

static inline void blkid_probe_start(blkid_probe pr)
{
	if (pr) {
		DBG(LOWPROBE, ul_debug("%p: start probe", pr));
		pr->cur_chain = NULL;
		pr->prob_flags = 0;
		blkid_probe_set_wiper(pr, 0, 0);
	}
}

static inline void blkid_probe_end(blkid_probe pr)
{
	if (pr) {
		DBG(LOWPROBE, ul_debug("%p: end probe", pr));
		pr->cur_chain = NULL;
		pr->prob_flags = 0;
		blkid_probe_set_wiper(pr, 0, 0);
	}
}

/**
 * blkid_do_probe:
 * @pr: prober
 *
 * Calls probing functions in all enabled chains. The superblocks chain is
 * enabled by default. The blkid_do_probe() stores result from only one
 * probing function. It's necessary to call this routine in a loop to get
 * results from all probing functions in all chains. The probing is reset
 * by blkid_reset_probe() or by filter functions.
 *
 * This is string-based NAME=value interface only.
 *
 * <example>
 *   <title>basic case - use the first result only</title>
 *   <programlisting>
 *	if (blkid_do_probe(pr) == 0) {
 *		int nvals = blkid_probe_numof_values(pr);
 *		for (n = 0; n < nvals; n++) {
 *			if (blkid_probe_get_value(pr, n, &name, &data, &len) == 0)
 *				printf("%s = %s\n", name, data);
 *		}
 *	}
 *  </programlisting>
 * </example>
 *
 * <example>
 *   <title>advanced case - probe for all signatures</title>
 *   <programlisting>
 *	while (blkid_do_probe(pr) == 0) {
 *		int nvals = blkid_probe_numof_values(pr);
 *		...
 *	}
 *  </programlisting>
 * </example>
 *
 * See also blkid_reset_probe().
 *
 * Returns: 0 on success, 1 when probing is done and -1 in case of error.
 */
int blkid_do_probe(blkid_probe pr)
{
	int rc = 1;

	if (!pr)
		return -1;

	if (pr->flags & BLKID_FL_NOSCAN_DEV)
		return 1;

	do {
		struct blkid_chain *chn = pr->cur_chain;

		if (!chn) {
			blkid_probe_start(pr);
			chn = pr->cur_chain = &pr->chains[0];
		}
		/* we go to the next chain only when the previous probing
		 * result was nothing (rc == 1) and when the current chain is
		 * disabled or we are at end of the current chain (chain->idx +
		 * 1 == sizeof chain) or the current chain bailed out right at
		 * the start (chain->idx == -1)
		 */
		else if (rc == 1 && (chn->enabled == FALSE ||
				     chn->idx + 1 == (int) chn->driver->nidinfos ||
				     chn->idx == -1)) {

			size_t idx = chn->driver->id + 1;

			if (idx < BLKID_NCHAINS)
				chn = pr->cur_chain = &pr->chains[idx];
			else {
				blkid_probe_end(pr);
				return 1;	/* all chains already probed */
			}
		}

		chn->binary = FALSE;		/* for sure... */

		DBG(LOWPROBE, ul_debug("chain probe %s %s (idx=%d)",
				chn->driver->name,
				chn->enabled? "ENABLED" : "DISABLED",
				chn->idx));

		if (!chn->enabled)
			continue;

		/* rc: -1 = error, 0 = success, 1 = no result */
		rc = chn->driver->probe(pr, chn);

	} while (rc == 1);

	return rc;
}

/**
 * blkid_do_wipe:
 * @pr: prober
 * @dryrun: if TRUE then don't touch the device.
 *
 * This function erases the current signature detected by @pr. The @pr has to
 * be open in O_RDWR mode, BLKID_SUBLKS_MAGIC or/and BLKID_PARTS_MAGIC flags
 * has to be enabled (if you want to errase also superblock with broken check
 * sums then use BLKID_SUBLKS_BADCSUM too).
 *
 * After successful signature removing the @pr prober will be moved one step
 * back and the next blkid_do_probe() call will again call previously called
 * probing function.
 *
 *  <example>
 *  <title>wipe all filesystems or raids from the device</title>
 *   <programlisting>
 *      fd = open(devname, O_RDWR|O_CLOEXEC);
 *      blkid_probe_set_device(pr, fd, 0, 0);
 *
 *      blkid_probe_enable_superblocks(pr, 1);
 *      blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_MAGIC);
 *
 *	while (blkid_do_probe(pr) == 0)
 *		blkid_do_wipe(pr, FALSE);
 *  </programlisting>
 * </example>
 *
 * See also blkid_probe_step_back() if you cannot use this build-in wipe
 * function, but you want to use libblkid probing as a source for wiping.
 *
 * Returns: 0 on success, and -1 in case of error.
 */
int blkid_do_wipe(blkid_probe pr, int dryrun)
{
	const char *off = NULL;
	size_t len = 0;
	uint64_t offset, l;
	char buf[BUFSIZ];
	int fd, rc = 0;
	struct blkid_chain *chn;

	if (!pr)
		return -1;

	chn = pr->cur_chain;
	if (!chn)
		return -1;

	switch (chn->driver->id) {
	case BLKID_CHAIN_SUBLKS:
		rc = blkid_probe_lookup_value(pr, "SBMAGIC_OFFSET", &off, NULL);
		if (!rc)
			rc = blkid_probe_lookup_value(pr, "SBMAGIC", NULL, &len);
		break;
	case BLKID_CHAIN_PARTS:
		rc = blkid_probe_lookup_value(pr, "PTMAGIC_OFFSET", &off, NULL);
		if (!rc)
			rc = blkid_probe_lookup_value(pr, "PTMAGIC", NULL, &len);
		break;
	default:
		return 0;
	}

	if (rc || len == 0 || off == NULL)
		return 0;

	offset = strtoumax(off, NULL, 10);
	fd = blkid_probe_get_fd(pr);
	if (fd < 0)
		return -1;

	if (len > sizeof(buf))
		len = sizeof(buf);

	DBG(LOWPROBE, ul_debug(
	    "do_wipe [offset=0x%jx (%ju), len=%zd, chain=%s, idx=%d, dryrun=%s]\n",
	    offset, offset, len, chn->driver->name, chn->idx, dryrun ? "yes" : "not"));

	l = blkid_llseek(fd, offset, SEEK_SET);
	if (l == (off_t) -1)
		return -1;

	memset(buf, 0, len);

	if (!dryrun && len) {
		if (write_all(fd, buf, len))
			return -1;
		fsync(fd);
		return blkid_probe_step_back(pr);
	}

	return 0;
}

/**
 * blkid_probe_step_back:
 * @pr: prober
 *
 * This function move pointer to the probing chain one step back -- it means
 * that the previously used probing function will be called again in the next
 * blkid_do_probe() call.
 *
 * This is necessary for example if you erase or modify on-disk superblock
 * according to the current libblkid probing result.
 *
 * <example>
 *  <title>wipe all superblock, but use libblkid only for probing</title>
 *  <programlisting>
 *      pr = blkid_new_probe_from_filename(devname);
 *
 *      blkid_probe_enable_superblocks(pr, 1);
 *      blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_MAGIC);
 *
 *      blkid_probe_enable_partitions(pr, 1);
 *      blkid_probe_set_partitions_flags(pr, BLKID_PARTS_MAGIC);
 *
 *	while (blkid_do_probe(pr) == 0) {
 *		const char *ostr = NULL;
 *		size_t len = 0;
 *
 *		// superblocks
 *		if (blkid_probe_lookup_value(pr, "SBMAGIC_OFFSET", &ostr, NULL) == 0)
 *			blkid_probe_lookup_value(pr, "SBMAGIC", NULL, &len);
 *
 *		// partition tables
 *		if (len == 0 && blkid_probe_lookup_value(pr, "PTMAGIC_OFFSET", &ostr, NULL) == 0)
 *			blkid_probe_lookup_value(pr, "PTMAGIC", NULL, &len);
 *
 *		if (!len || !str)
 *			continue;
 *
 *		// convert ostr to the real offset by off = strtoll(ostr, NULL, 10);
 *              // use your stuff to errase @len bytes at the @off
 *              ....
 *
 *		// retry the last probing to check for backup superblocks ..etc.
 *              blkid_probe_step_back(pr);
 *	}
 *  </programlisting>
 * </example>
 *
 * Returns: 0 on success, and -1 in case of error.
 */
int blkid_probe_step_back(blkid_probe pr)
{
	struct blkid_chain *chn;

	if (!pr)
		return -1;

	chn = pr->cur_chain;
	if (!chn)
		return -1;

	blkid_probe_reset_buffer(pr);

	if (chn->idx >= 0) {
		chn->idx--;
		DBG(LOWPROBE, ul_debug("step back: moving %s chain index to %d",
			chn->driver->name,
			chn->idx));
	}

	if (chn->idx == -1) {
		/* blkid_do_probe() goes to the next chain if the index
		 * of the current chain is -1, so we have to set the
		 * chain pointer to the previous chain.
		 */
		size_t idx = chn->driver->id > 0 ? chn->driver->id - 1 : 0;

		DBG(LOWPROBE, ul_debug("step back: moving to previous chain"));

		if (idx > 0)
			pr->cur_chain = &pr->chains[idx];
		else if (idx == 0)
			pr->cur_chain = NULL;
	}

	return 0;
}

/**
 * blkid_do_safeprobe:
 * @pr: prober
 *
 * This function gathers probing results from all enabled chains and checks
 * for ambivalent results (e.g. more filesystems on the device).
 *
 * This is string-based NAME=value interface only.
 *
 * Note about suberblocks chain -- the function does not check for filesystems
 * when a RAID signature is detected.  The function also does not check for
 * collision between RAIDs. The first detected RAID is returned. The function
 * checks for collision between partition table and RAID signature -- it's
 * recommended to enable partitions chain together with superblocks chain.
 *
 * Returns: 0 on success, 1 if nothing is detected, -2 if ambivalen result is
 * detected and -1 on case of error.
 */
int blkid_do_safeprobe(blkid_probe pr)
{
	int i, count = 0, rc = 0;

	if (!pr)
		return -1;
	if (pr->flags & BLKID_FL_NOSCAN_DEV)
		return 1;

	blkid_probe_start(pr);

	for (i = 0; i < BLKID_NCHAINS; i++) {
		struct blkid_chain *chn;

		chn = pr->cur_chain = &pr->chains[i];
		chn->binary = FALSE;		/* for sure... */

		DBG(LOWPROBE, ul_debug("chain safeprobe %s %s",
				chn->driver->name,
				chn->enabled? "ENABLED" : "DISABLED"));

		if (!chn->enabled)
			continue;

		blkid_probe_chain_reset_position(chn);

		rc = chn->driver->safeprobe(pr, chn);

		blkid_probe_chain_reset_position(chn);

		/* rc: -2 ambivalent, -1 = error, 0 = success, 1 = no result */
		if (rc < 0)
			goto done;	/* error */
		if (rc == 0)
			count++;	/* success */
	}

done:
	blkid_probe_end(pr);
	if (rc < 0)
		return rc;
	return count ? 0 : 1;
}

/**
 * blkid_do_fullprobe:
 * @pr: prober
 *
 * This function gathers probing results from all enabled chains. Same as
 * blkid_do_safeprobe() but does not check for collision between probing
 * result.
 *
 * This is string-based NAME=value interface only.
 *
 * Returns: 0 on success, 1 if nothing is detected or -1 on case of error.
 */
int blkid_do_fullprobe(blkid_probe pr)
{
	int i, count = 0, rc = 0;

	if (!pr)
		return -1;
	if (pr->flags & BLKID_FL_NOSCAN_DEV)
		return 1;

	blkid_probe_start(pr);

	for (i = 0; i < BLKID_NCHAINS; i++) {
		struct blkid_chain *chn;

		chn = pr->cur_chain = &pr->chains[i];
		chn->binary = FALSE;		/* for sure... */

		DBG(LOWPROBE, ul_debug("chain fullprobe %s: %s",
				chn->driver->name,
				chn->enabled? "ENABLED" : "DISABLED"));

		if (!chn->enabled)
			continue;

		blkid_probe_chain_reset_position(chn);

		rc = chn->driver->probe(pr, chn);

		blkid_probe_chain_reset_position(chn);

		/* rc: -1 = error, 0 = success, 1 = no result */
		if (rc < 0)
			goto done;	/* error */
		if (rc == 0)
			count++;	/* success */
	}

done:
	blkid_probe_end(pr);
	if (rc < 0)
		return rc;
	return count ? 0 : 1;
}

/* same sa blkid_probe_get_buffer() but works with 512-sectors */
unsigned char *blkid_probe_get_sector(blkid_probe pr, unsigned int sector)
{
	return pr ? blkid_probe_get_buffer(pr,
			((uint64_t) sector) << 9, 0x200) : NULL;
}

struct blkid_prval *blkid_probe_assign_value(
			blkid_probe pr, const char *name)
{
	struct blkid_prval *v;
	if (!name)
		return NULL;

	v = blkid_probe_new_value();
	if (!v)
		return NULL;

	v->name = name;
	v->chain = pr->cur_chain;
	list_add_tail(&v->prvals, &pr->values);

	DBG(LOWPROBE, ul_debug("assigning %s [%s]", name, v->chain->driver->name));
	return v;
}

static struct blkid_prval *blkid_probe_new_value(void)
{
	struct blkid_prval *v = calloc(1, sizeof(struct blkid_prval));
	if (!v)
		return NULL;

	INIT_LIST_HEAD(&v->prvals);

	return v;
}

/* Note that value data is always terminated by zero to keep things robust,
 * this extra zero is not count to the value length. It's caller responsibility
 * to set proper value length (for strings we count terminator to the length,
 * for binary data it's without terminator).
 */
int blkid_probe_value_set_data(struct blkid_prval *v,
		unsigned char *data, size_t len)
{
	v->data = calloc(1, len + 1);	/* always terminate by \0 */

	if (!v->data)
		return -ENOMEM;
	memcpy(v->data, data, len);
	v->len = len;
	return 0;
}

int blkid_probe_set_value(blkid_probe pr, const char *name,
		unsigned char *data, size_t len)
{
	struct blkid_prval *v;

	v = blkid_probe_assign_value(pr, name);
	if (!v)
		return -1;

	return blkid_probe_value_set_data(v, data, len);
}

int blkid_probe_vsprintf_value(blkid_probe pr, const char *name,
		const char *fmt, va_list ap)
{
	struct blkid_prval *v;
	ssize_t len;

	v = blkid_probe_assign_value(pr, name);
	if (!v)
		return -ENOMEM;

	len = vasprintf((char **) &v->data, fmt, ap);

	if (len <= 0) {
		blkid_probe_free_value(v);
		return len == 0 ? -EINVAL : -ENOMEM;
	}
	v->len = len + 1;
	return 0;
}

int blkid_probe_sprintf_value(blkid_probe pr, const char *name,
		const char *fmt, ...)
{
	int rc;
	va_list ap;

	va_start(ap, fmt);
	rc = blkid_probe_vsprintf_value(pr, name, fmt, ap);
	va_end(ap);

	return rc;
}

int blkid_probe_set_magic(blkid_probe pr, uint64_t offset,
			size_t len, unsigned char *magic)
{
	int rc = 0;
	struct blkid_chain *chn = blkid_probe_get_chain(pr);

	if (!chn || !magic || !len || chn->binary)
		return 0;

	switch (chn->driver->id) {
	case BLKID_CHAIN_SUBLKS:
		if (!(chn->flags & BLKID_SUBLKS_MAGIC))
			return 0;
		rc = blkid_probe_set_value(pr, "SBMAGIC", magic, len);
		if (!rc)
			rc = blkid_probe_sprintf_value(pr,
					"SBMAGIC_OFFSET", "%llu", (unsigned long long)offset);
		break;
	case BLKID_CHAIN_PARTS:
		if (!(chn->flags & BLKID_PARTS_MAGIC))
			return 0;
		rc = blkid_probe_set_value(pr, "PTMAGIC", magic, len);
		if (!rc)
			rc = blkid_probe_sprintf_value(pr,
					"PTMAGIC_OFFSET", "%llu", (unsigned long long)offset);
		break;
	default:
		break;
	}

	return rc;
}

int blkid_probe_verify_csum(blkid_probe pr, uint64_t csum, uint64_t expected)
{
	if (csum != expected) {
		struct blkid_chain *chn = blkid_probe_get_chain(pr);

		DBG(LOWPROBE, ul_debug(
				"incorrect checksum for type %s,"
				" got %jX, expected %jX",
				blkid_probe_get_probername(pr),
				csum, expected));
		/*
		 * Accept bad checksum if BLKID_SUBLKS_BADCSUM flags is set
		 */
		if (chn->driver->id == BLKID_CHAIN_SUBLKS
		    && (chn->flags & BLKID_SUBLKS_BADCSUM)) {
			blkid_probe_set_value(pr, "SBBADCSUM", (unsigned char *) "1", 2);
			goto accept;
		}
		return 0;	/* bad checksum */
	}

accept:
	return 1;
}

/**
 * blkid_probe_get_devno:
 * @pr: probe
 *
 * Returns: block device number, or 0 for regular files.
 */
dev_t blkid_probe_get_devno(blkid_probe pr)
{
	return pr->devno;
}

/**
 * blkid_probe_get_wholedisk_devno:
 * @pr: probe
 *
 * Returns: device number of the wholedisk, or 0 for regular files.
 */
dev_t blkid_probe_get_wholedisk_devno(blkid_probe pr)
{
	if (!pr->disk_devno) {
		dev_t devno, disk_devno = 0;

		devno = blkid_probe_get_devno(pr);
		if (!devno)
			return 0;

		if (blkid_devno_to_wholedisk(devno, NULL, 0, &disk_devno) == 0)
			pr->disk_devno = disk_devno;
	}
	return pr->disk_devno;
}

/**
 * blkid_probe_is_wholedisk:
 * @pr: probe
 *
 * Returns: 1 if the device is whole-disk or 0.
 */
int blkid_probe_is_wholedisk(blkid_probe pr)
{
	dev_t devno, disk_devno;

	devno = blkid_probe_get_devno(pr);
	if (!devno)
		return 0;

	disk_devno = blkid_probe_get_wholedisk_devno(pr);
	if (!disk_devno)
		return 0;

	return devno == disk_devno;
}

blkid_probe blkid_probe_get_wholedisk_probe(blkid_probe pr)
{
	dev_t disk;

	if (blkid_probe_is_wholedisk(pr))
		return NULL;			/* this is not partition */

	if (pr->parent)
		/* this is cloned blkid_probe, use parent's stuff */
		return blkid_probe_get_wholedisk_probe(pr->parent);

	disk = blkid_probe_get_wholedisk_devno(pr);

	if (pr->disk_probe && pr->disk_probe->devno != disk) {
		/* we have disk prober, but for another disk... close it */
		blkid_free_probe(pr->disk_probe);
		pr->disk_probe = NULL;
	}

	if (!pr->disk_probe) {
		/* Open a new disk prober */
		char *disk_path = blkid_devno_to_devname(disk);

		if (!disk_path)
			return NULL;

		DBG(LOWPROBE, ul_debug("allocate a wholedisk probe"));

		pr->disk_probe = blkid_new_probe_from_filename(disk_path);
		free(disk_path);

		if (!pr->disk_probe)
			return NULL;	/* ENOMEM? */
		if (pr->conf)
			blkid_probe_set_config(pr->disk_probe, pr->conf);
	}

	return pr->disk_probe;
}

/**
 * blkid_probe_get_size:
 * @pr: probe
 *
 * This function returns size of probing area as defined by blkid_probe_set_device().
 * If the size of the probing area is unrestricted then this function returns
 * the real size of device. See also blkid_get_dev_size().
 *
 * Returns: size in bytes or -1 in case of error.
 */
blkid_loff_t blkid_probe_get_size(blkid_probe pr)
{
	return pr ? (blkid_loff_t) pr->size : -1;
}

/**
 * blkid_probe_get_offset:
 * @pr: probe
 *
 * This function returns offset of probing area as defined by blkid_probe_set_device().
 *
 * Returns: offset in bytes or -1 in case of error.
 */
blkid_loff_t blkid_probe_get_offset(blkid_probe pr)
{
	return pr ? (blkid_loff_t) pr->off : -1;
}

/**
 * blkid_probe_get_fd:
 * @pr: probe
 *
 * Returns: file descriptor for assigned device/file or -1 in case of error.
 */
int blkid_probe_get_fd(blkid_probe pr)
{
	return pr ? pr->fd : -1;
}

/**
 * blkid_probe_get_sectorsize:
 * @pr: probe or NULL (for NULL returns 512)
 *
 * Returns: block device logical sector size (BLKSSZGET ioctl, default 512).
 */
unsigned int blkid_probe_get_sectorsize(blkid_probe pr)
{
	if (!pr)
		return DEFAULT_SECTOR_SIZE;  /*... and good luck! */

	if (pr->blkssz)
		return pr->blkssz;

	if (S_ISBLK(pr->mode) &&
	    blkdev_get_sector_size(pr->fd, (int *) &pr->blkssz) == 0)
		return pr->blkssz;

	pr->blkssz = DEFAULT_SECTOR_SIZE;
	return pr->blkssz;
}

/**
 * blkid_probe_get_sectors:
 * @pr: probe
 *
 * Returns: 512-byte sector count or -1 in case of error.
 */
blkid_loff_t blkid_probe_get_sectors(blkid_probe pr)
{
	return pr ? (blkid_loff_t) (pr->size >> 9) : -1;
}

/**
 * blkid_probe_numof_values:
 * @pr: probe
 *
 * Returns: number of values in probing result or -1 in case of error.
 */
int blkid_probe_numof_values(blkid_probe pr)
{
	int i = 0;
	struct list_head *p;
	if (!pr)
		return -1;

	list_for_each(p, &pr->values)
		++i;
	return i;
}

/**
 * blkid_probe_get_value:
 * @pr: probe
 * @num: wanted value in range 0..N, where N is blkid_probe_numof_values() - 1
 * @name: pointer to return value name or NULL
 * @data: pointer to return value data or NULL
 * @len: pointer to return value length or NULL
 *
 * Note, the @len returns length of the @data, including the terminating
 * '\0' character.
 *
 * Returns: 0 on success, or -1 in case of error.
 */
int blkid_probe_get_value(blkid_probe pr, int num, const char **name,
			const char **data, size_t *len)
{
	struct blkid_prval *v = __blkid_probe_get_value(pr, num);

	if (!v)
		return -1;
	if (name)
		*name = v->name;
	if (data)
		*data = (char *) v->data;
	if (len)
		*len = v->len;

	DBG(LOWPROBE, ul_debug("returning %s value", v->name));
	return 0;
}

/**
 * blkid_probe_lookup_value:
 * @pr: probe
 * @name: name of value
 * @data: pointer to return value data or NULL
 * @len: pointer to return value length or NULL
 *
 * Note, the @len returns length of the @data, including the terminating
 * '\0' character.
 *
 * Returns: 0 on success, or -1 in case of error.
 */
int blkid_probe_lookup_value(blkid_probe pr, const char *name,
			const char **data, size_t *len)
{
	struct blkid_prval *v = __blkid_probe_lookup_value(pr, name);

	if (!v)
		return -1;
	if (data)
		*data = (char *) v->data;
	if (len)
		*len = v->len;
	return 0;
}

/**
 * blkid_probe_has_value:
 * @pr: probe
 * @name: name of value
 *
 * Returns: 1 if value exist in probing result, otherwise 0.
 */
int blkid_probe_has_value(blkid_probe pr, const char *name)
{
	if (blkid_probe_lookup_value(pr, name, NULL, NULL) == 0)
		return 1;
	return 0;
}

struct blkid_prval *blkid_probe_last_value(blkid_probe pr)
{
	if (!pr || list_empty(&pr->values))
		return NULL;

	return list_last_entry(&pr->values, struct blkid_prval, prvals);
}


struct blkid_prval *__blkid_probe_get_value(blkid_probe pr, int num)
{
	int i = 0;
	struct list_head *p;

	if (!pr || num < 0)
		return NULL;

	list_for_each(p, &pr->values) {
		if (i++ != num)
			continue;
		return list_entry(p, struct blkid_prval, prvals);
	}
	return NULL;
}

struct blkid_prval *__blkid_probe_lookup_value(blkid_probe pr, const char *name)
{
	struct list_head *p;

	if (!pr || list_empty(&pr->values) || !name)
		return NULL;

	list_for_each(p, &pr->values) {
		struct blkid_prval *v = list_entry(p, struct blkid_prval,
						prvals);

		if (v->name && strcmp(name, v->name) == 0) {
			DBG(LOWPROBE, ul_debug("returning %s value", v->name));
			return v;
		}
	}
	return NULL;
}


/* converts DCE UUID (uuid[16]) to human readable string
 * - the @len should be always 37 */
#ifdef HAVE_LIBUUID
void blkid_unparse_uuid(const unsigned char *uuid, char *str,
			size_t len __attribute__((__unused__)))
{
	uuid_unparse(uuid, str);
}
#else
void blkid_unparse_uuid(const unsigned char *uuid, char *str, size_t len)
{
	snprintf(str, len,
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		uuid[0], uuid[1], uuid[2], uuid[3],
		uuid[4], uuid[5],
		uuid[6], uuid[7],
		uuid[8], uuid[9],
		uuid[10], uuid[11], uuid[12], uuid[13], uuid[14],uuid[15]);
}
#endif

/* like uuid_is_null() from libuuid, but works with arbitrary size of UUID */
int blkid_uuid_is_empty(const unsigned char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (buf[i])
			return 0;
	return 1;
}

/* Removes whitespace from the right-hand side of a string (trailing
 * whitespace).
 *
 * Returns size of the new string (without \0).
 */
size_t blkid_rtrim_whitespace(unsigned char *str)
{
	return rtrim_whitespace(str);
}

/* Removes whitespace from the left-hand side of a string.
 *
 * Returns size of the new string (without \0).
 */
size_t blkid_ltrim_whitespace(unsigned char *str)
{
	return ltrim_whitespace(str);
}

/*
 * Some mkfs-like utils wipe some parts (usually begin) of the device.
 * For example LVM (pvcreate) or mkswap(8). This information could be used
 * for later resolution to conflicts between superblocks.
 *
 * For example we found valid LVM superblock, LVM wipes 8KiB at the begin of
 * the device. If we found another signature (for example MBR) within the
 * wiped area then the signature has been added later and LVM superblock
 * should be ignore.
 *
 * Note that this heuristic is not 100% reliable, for example "pvcreate --zero
 * n" allows to keep the begin of the device unmodified. It's probably better
 * to use this heuristic for conflicts between superblocks and partition tables
 * than for conflicts between filesystem superblocks -- existence of unwanted
 * partition table is very unusual, because PT is pretty visible (parsed and
 * interpreted by kernel).
 *
 * Note that we usually expect only one signature on the device, it means that
 * we have to remember only one wiped area from previously successfully
 * detected signature.
 *
 * blkid_probe_set_wiper() -- defines wiped area (e.g. LVM)
 * blkid_probe_use_wiper() -- try to use area (e.g. MBR)
 *
 * Note that there is not relation between _wiper and blkid_to_wipe().
 *
 */
void blkid_probe_set_wiper(blkid_probe pr, uint64_t off, uint64_t size)
{
	struct blkid_chain *chn;

	if (!pr)
		return;

	if (!size) {
		DBG(LOWPROBE, ul_debug("zeroize wiper"));
		pr->wipe_size = pr->wipe_off = 0;
		pr->wipe_chain = NULL;
		return;
	}

	chn = pr->cur_chain;

	if (!chn || !chn->driver ||
	    chn->idx < 0 || (size_t) chn->idx >= chn->driver->nidinfos)
		return;

	pr->wipe_size = size;
	pr->wipe_off = off;
	pr->wipe_chain = chn;

	DBG(LOWPROBE,
		ul_debug("wiper set to %s::%s off=%jd size=%jd",
			chn->driver->name,
			chn->driver->idinfos[chn->idx]->name,
			pr->wipe_off, pr->wipe_size));
	return;
}

/*
 * Returns 1 if the <@off,@size> area was wiped
 */
int blkid_probe_is_wiped(blkid_probe pr, struct blkid_chain **chn, uint64_t off, uint64_t size)
{
	if (!pr || !size)
		return 0;

	if (pr->wipe_off <= off && off + size <= pr->wipe_off + pr->wipe_size) {
		if (chn)
			*chn = pr->wipe_chain;
		return 1;
	}
	return 0;
}

/*
 *  Try to use any area -- if the area has been previously wiped then the
 *  previous probing result should be ignored (reseted).
 */
void blkid_probe_use_wiper(blkid_probe pr, uint64_t off, uint64_t size)
{
	struct blkid_chain *chn = NULL;

	if (blkid_probe_is_wiped(pr, &chn, off, size) && chn) {
		DBG(LOWPROBE, ul_debug("previously wiped area modified "
				       " -- ignore previous results"));
		blkid_probe_set_wiper(pr, 0, 0);
		blkid_probe_chain_reset_values(pr, chn);
	}
}
