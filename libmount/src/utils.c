/*
 * Copyright (C) 2008-2009 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

/**
 * SECTION: utils
 * @title: Utils
 * @short_description: misc utils.
 */
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <blkid.h>

#include "strutils.h"
#include "pathnames.h"
#include "mountP.h"
#include "mangle.h"
#include "canonicalize.h"
#include "env.h"
#include "match.h"
#include "fileutils.h"
#include "statfs_magic.h"

int append_string(char **a, const char *b)
{
	size_t al, bl;
	char *tmp;

	assert(a);

	if (!b || !*b)
		return 0;
	if (!*a) {
		*a = strdup(b);
		return !*a ? -ENOMEM : 0;
	}

	al = strlen(*a);
	bl = strlen(b);

	tmp = realloc(*a, al + bl + 1);
	if (!tmp)
		return -ENOMEM;
	*a = tmp;
	memcpy((*a) + al, b, bl + 1);
	return 0;
}

/*
 * Return 1 if the file is not accessible or empty
 */
int is_file_empty(const char *name)
{
	struct stat st;
	assert(name);

	return (stat(name, &st) != 0 || st.st_size == 0);
}

int mnt_valid_tagname(const char *tagname)
{
	if (tagname && *tagname && (
	    strcmp("UUID", tagname) == 0 ||
	    strcmp("LABEL", tagname) == 0 ||
	    strcmp("PARTUUID", tagname) == 0 ||
	    strcmp("PARTLABEL", tagname) == 0))
		return 1;

	return 0;
}

/**
 * mnt_tag_is_valid:
 * @tag: NAME=value string
 *
 * Returns: 1 if the @tag is parsable and tag NAME= is supported by libmount, or 0.
 */
int mnt_tag_is_valid(const char *tag)
{
	char *t = NULL;
	int rc = tag && blkid_parse_tag_string(tag, &t, NULL) == 0
		     && mnt_valid_tagname(t);

	free(t);
	return rc;
}

int mnt_parse_offset(const char *str, size_t len, uintmax_t *res)
{
	char *p;
	int rc = 0;

	if (!str || !*str)
		return -EINVAL;

	p = strndup(str, len);
	if (!p)
		return -errno;

	if (strtosize(p, res))
		rc = -EINVAL;
	free(p);
	return rc;
}

/* used as a callback by bsearch in mnt_fstype_is_pseudofs() */
static int fstype_cmp(const void *v1, const void *v2)
{
	const char *s1 = *(const char **)v1;
	const char *s2 = *(const char **)v2;

	return strcmp(s1, s2);
}

int mnt_stat_mountpoint(const char *target, struct stat *st)
{
#ifdef AT_NO_AUTOMOUNT
	return fstatat(-1, target, st, AT_NO_AUTOMOUNT);
#else
	return stat(target, st);
#endif
}

/*
 * Note that the @target has to be an absolute path (so at least "/").  The
 * @filename returns an allocated buffer with the last path component, for example:
 *
 * mnt_chdir_to_parent("/mnt/test", &buf) ==> chdir("/mnt"), buf="test"
 */
int mnt_chdir_to_parent(const char *target, char **filename)
{
	char *buf, *parent, *last = NULL;
	char cwd[PATH_MAX];
	int rc = -EINVAL;

	if (!target || *target != '/')
		return -EINVAL;

	DBG(UTILS, ul_debug("moving to %s parent", target));

	buf = strdup(target);
	if (!buf)
		return -ENOMEM;

	if (*(buf + 1) != '\0') {
		last = stripoff_last_component(buf);
		if (!last)
			goto err;
	}

	parent = buf && *buf ? buf : "/";

	if (chdir(parent) == -1) {
		DBG(UTILS, ul_debug("failed to chdir to %s: %m", parent));
		rc = -errno;
		goto err;
	}
	if (!getcwd(cwd, sizeof(cwd))) {
		DBG(UTILS, ul_debug("failed to obtain current directory: %m"));
		rc = -errno;
		goto err;
	}
	if (strcmp(cwd, parent) != 0) {
		DBG(UTILS, ul_debug(
		    "unexpected chdir (expected=%s, cwd=%s)", parent, cwd));
		goto err;
	}

	DBG(CXT, ul_debug(
		"current directory moved to %s [last_component='%s']",
		parent, last));

	if (filename) {
		*filename = buf;

		if (!last || !*last)
			memcpy(*filename, ".", 2);
		else
			memmove(*filename, last, strlen(last) + 1);
	} else
		free(buf);
	return 0;
err:
	free(buf);
	return rc;
}

/*
 * Check if @path is on a read-only filesystem independently of file permissions.
 */
int mnt_is_readonly(const char *path)
{
	if (access(path, W_OK) == 0)
		return 0;
	if (errno == EROFS)
		return 1;
	if (errno != EACCES)
		return 0;

#ifdef HAVE_UTIMENSAT
	/*
	 * access(2) returns EACCES on read-only FS:
	 *
	 * - for set-uid application if one component of the path is not
	 *   accessible for the current rUID. (Note that euidaccess(2) does not
	 *   check for EROFS at all).
	 *
	 * - for a read-write filesystem with a read-only VFS node (aka -o remount,ro,bind)
	 */
	{
		struct timespec times[2];

		times[0].tv_nsec = UTIME_NOW;	/* atime */
		times[1].tv_nsec = UTIME_OMIT;	/* mtime */

		if (utimensat(AT_FDCWD, path, times, 0) == -1)
			return errno == EROFS;
	}
#endif
	return 0;
}

/**
 * mnt_mangle:
 * @str: string
 *
 * Encode @str to be compatible with fstab/mtab
 *
 * Returns: newly allocated string or NULL in case of error.
 */
char *mnt_mangle(const char *str)
{
	return mangle(str);
}

/**
 * mnt_unmangle:
 * @str: string
 *
 * Decode @str from fstab/mtab
 *
 * Returns: newly allocated string or NULL in case of error.
 */
char *mnt_unmangle(const char *str)
{
	return unmangle(str, NULL);
}

/**
 * mnt_fstype_is_pseudofs:
 * @type: filesystem name
 *
 * Returns: 1 for filesystems like proc, sysfs, ... or 0.
 */
int mnt_fstype_is_pseudofs(const char *type)
{
	/* This array must remain sorted when adding new fstypes */
	static const char *pseudofs[] = {
		"anon_inodefs",
		"autofs",
		"bdev",
		"binfmt_misc",
		"cgroup",
		"cgroup2",
		"configfs",
		"cpuset",
		"debugfs",
		"devfs",
		"devpts",
		"devtmpfs",
		"dlmfs",
		"efivarfs",
		"fuse.gvfs-fuse-daemon",
		"fusectl",
		"hugetlbfs",
		"mqueue",
		"nfsd",
		"none",
		"overlay",
		"pipefs",
		"proc",
		"pstore",
		"ramfs",
		"rootfs",
		"rpc_pipefs",
		"securityfs",
		"sockfs",
		"spufs",
		"sysfs",
		"tmpfs"
	};

	assert(type);

	return !(bsearch(&type, pseudofs, ARRAY_SIZE(pseudofs),
				sizeof(char*), fstype_cmp) == NULL);
}

/**
 * mnt_fstype_is_netfs:
 * @type: filesystem name
 *
 * Returns: 1 for filesystems like cifs, nfs, ... or 0.
 */
int mnt_fstype_is_netfs(const char *type)
{
	if (strcmp(type, "cifs")   == 0 ||
	    strcmp(type, "smbfs")  == 0 ||
	    strncmp(type,"nfs", 3) == 0 ||
	    strcmp(type, "afs")    == 0 ||
	    strcmp(type, "ncpfs")  == 0 ||
	    strncmp(type,"9p", 2)  == 0)
		return 1;
	return 0;
}

const char *mnt_statfs_get_fstype(struct statfs *vfs)
{
	assert(vfs);

	switch (vfs->f_type) {
	case STATFS_ADFS_MAGIC:		return "adfs";
	case STATFS_AFFS_MAGIC:		return "affs";
	case STATFS_AFS_MAGIC:		return "afs";
	case STATFS_AUTOFS_MAGIC:	return "autofs";
	case STATFS_BDEVFS_MAGIC:	return "bdev";
	case STATFS_BEFS_MAGIC:		return "befs";
	case STATFS_BFS_MAGIC:		return "befs";
	case STATFS_BINFMTFS_MAGIC:	return "binfmt_misc";
	case STATFS_BTRFS_MAGIC:	return "btrfs";
	case STATFS_CEPH_MAGIC:		return "ceph";
	case STATFS_CGROUP_MAGIC:	return "cgroup";
	case STATFS_CIFS_MAGIC:		return "cifs";
	case STATFS_CODA_MAGIC:		return "coda";
	case STATFS_CONFIGFS_MAGIC:	return "configfs";
	case STATFS_CRAMFS_MAGIC:	return "cramfs";
	case STATFS_DEBUGFS_MAGIC:	return "debugfs";
	case STATFS_DEVPTS_MAGIC:	return "devpts";
	case STATFS_ECRYPTFS_MAGIC:	return "ecryptfs";
	case STATFS_EFIVARFS_MAGIC:	return "efivarfs";
	case STATFS_EFS_MAGIC:		return "efs";
	case STATFS_EXOFS_MAGIC:	return "exofs";
	case STATFS_EXT4_MAGIC:		return "ext4";	   /* all extN use the same magic */
	case STATFS_F2FS_MAGIC:		return "f2fs";
	case STATFS_FUSE_MAGIC:		return "fuse";
	case STATFS_FUTEXFS_MAGIC:	return "futexfs";
	case STATFS_GFS2_MAGIC:		return "gfs2";
	case STATFS_HFSPLUS_MAGIC:	return "hfsplus";
	case STATFS_HOSTFS_MAGIC:	return "hostfs";
	case STATFS_HPFS_MAGIC:		return "hpfs";
	case STATFS_HPPFS_MAGIC:	return "hppfs";
	case STATFS_HUGETLBFS_MAGIC:	return "hugetlbfs";
	case STATFS_ISOFS_MAGIC:	return "iso9660";
	case STATFS_JFFS2_MAGIC:	return "jffs2";
	case STATFS_JFS_MAGIC:		return "jfs";
	case STATFS_LOGFS_MAGIC:	return "logfs";
	case STATFS_MINIX2_MAGIC:
	case STATFS_MINIX2_MAGIC2:
	case STATFS_MINIX3_MAGIC:
	case STATFS_MINIX_MAGIC:
	case STATFS_MINIX_MAGIC2:	return "minix";
	case STATFS_MQUEUE_MAGIC:	return "mqueue";
	case STATFS_MSDOS_MAGIC:	return "vfat";
	case STATFS_NCP_MAGIC:		return "ncp";
	case STATFS_NFS_MAGIC:		return "nfs";
	case STATFS_NILFS_MAGIC:	return "nilfs2";
	case STATFS_NTFS_MAGIC:		return "ntfs";
	case STATFS_OCFS2_MAGIC:	return "ocfs2";
	case STATFS_OMFS_MAGIC:		return "omfs";
	case STATFS_OPENPROMFS_MAGIC:	return "openpromfs";
	case STATFS_PIPEFS_MAGIC:	return "pipefs";
	case STATFS_PROC_MAGIC:		return "proc";
	case STATFS_PSTOREFS_MAGIC:	return "pstore";
	case STATFS_QNX4_MAGIC:		return "qnx4";
	case STATFS_QNX6_MAGIC:		return "qnx6";
	case STATFS_RAMFS_MAGIC:	return "ramfs";
	case STATFS_REISERFS_MAGIC:	return "reiser4";
	case STATFS_ROMFS_MAGIC:	return "romfs";
	case STATFS_SECURITYFS_MAGIC:	return "securityfs";
	case STATFS_SELINUXFS_MAGIC:	return "selinuxfs";
	case STATFS_SMACKFS_MAGIC:	return "smackfs";
	case STATFS_SMB_MAGIC:		return "smb";
	case STATFS_SOCKFS_MAGIC:	return "sockfs";
	case STATFS_SQUASHFS_MAGIC:	return "squashfs";
	case STATFS_SYSFS_MAGIC:	return "sysfs";
	case STATFS_TMPFS_MAGIC:	return "tmpfs";
	case STATFS_UBIFS_MAGIC:	return "ubifs";
	case STATFS_UDF_MAGIC:		return "udf";
	case STATFS_UFS2_MAGIC:
	case STATFS_UFS_MAGIC:		return "ufs";
	case STATFS_V9FS_MAGIC:		return "9p";
	case STATFS_VXFS_MAGIC:		return "vxfs";
	case STATFS_XENFS_MAGIC:	return "xenfs";
	case STATFS_XFS_MAGIC:		return "xfs";
	default:
		break;
	}

	return NULL;
}


/**
 * mnt_match_fstype:
 * @type: filesystem type
 * @pattern: filesystem name or comma delimited list of names
 *
 * The @pattern list of filesystems can be prefixed with a global
 * "no" prefix to invert matching of the whole list. The "no" could
 * also be used for individual items in the @pattern list. So,
 * "nofoo,bar" has the same meaning as "nofoo,nobar".
 *
 * "bar"  : "nofoo,bar"		-> False   (global "no" prefix)
 *
 * "bar"  : "foo,bar"		-> True
 *
 * "bar" : "foo,nobar"		-> False
 *
 * Returns: 1 if type is matching, else 0. This function also returns
 *          0 if @pattern is NULL and @type is non-NULL.
 */
int mnt_match_fstype(const char *type, const char *pattern)
{
	return match_fstype(type, pattern);
}


/* Returns 1 if needle found or noneedle not found in haystack
 * Otherwise returns 0
 */
static int check_option(const char *haystack, size_t len,
			const char *needle, size_t needle_len)
{
	const char *p;
	int no = 0;

	if (needle_len >= 1 && *needle == '+') {
		needle++;
		needle_len--;
	} else if (needle_len >= 2 && !strncmp(needle, "no", 2)) {
		no = 1;
		needle += 2;
		needle_len -= 2;
	}

	for (p = haystack; p && p < haystack + len; p++) {
		char *sep = strchr(p, ',');
		size_t plen = sep ? (size_t) (sep - p) :
				    len - (p - haystack);

		if (plen == needle_len) {
			if (!strncmp(p, needle, plen))
				return !no;	/* foo or nofoo was found */
		}
		p += plen;
	}

	return no;  /* foo or nofoo was not found */
}

/**
 * mnt_match_options:
 * @optstr: options string
 * @pattern: comma delimited list of options
 *
 * The "no" could be used for individual items in the @options list. The "no"
 * prefix does not have a global meaning.
 *
 * Unlike fs type matching, nonetdev,user and nonetdev,nouser have
 * DIFFERENT meanings; each option is matched explicitly as specified.
 *
 * The "no" prefix interpretation could be disabled by the "+" prefix, for example
 * "+noauto" matches if @optstr literally contains the "noauto" string.
 *
 * "xxx,yyy,zzz" : "nozzz"	-> False
 *
 * "xxx,yyy,zzz" : "xxx,noeee"	-> True
 *
 * "bar,zzz"     : "nofoo"      -> True
 *
 * "nofoo,bar"   : "+nofoo"     -> True
 *
 * "bar,zzz"     : "+nofoo"     -> False
 *
 *
 * Returns: 1 if pattern is matching, else 0. This function also returns 0
 *          if @pattern is NULL and @optstr is non-NULL.
 */
int mnt_match_options(const char *optstr, const char *pattern)
{
	const char *p;
	size_t len, optstr_len = 0;

	if (!pattern && !optstr)
		return 1;
	if (!pattern)
		return 0;

	len = strlen(pattern);
	if (optstr)
		optstr_len = strlen(optstr);

	for (p = pattern; p < pattern + len; p++) {
		char *sep = strchr(p, ',');
		size_t plen = sep ? (size_t) (sep - p) :
				    len - (p - pattern);

		if (!plen)
			continue; /* if two ',' appear in a row */

		if (!check_option(optstr, optstr_len, p, plen))
			return 0; /* any match failure means failure */

		p += plen;
	}

	/* no match failures in list means success */
	return 1;
}

void mnt_free_filesystems(char **filesystems)
{
	char **p;

	if (!filesystems)
		return;
	for (p = filesystems; *p; p++)
		free(*p);
	free(filesystems);
}

static int add_filesystem(char ***filesystems, char *name)
{
	int n = 0;

	assert(filesystems);
	assert(name);

	if (*filesystems) {
		char **p;
		for (n = 0, p = *filesystems; *p; p++, n++) {
			if (strcmp(*p, name) == 0)
				return 0;
		}
	}

	#define MYCHUNK	16

	if (n == 0 || !((n + 1) % MYCHUNK)) {
		size_t items = ((n + 1 + MYCHUNK) / MYCHUNK) * MYCHUNK;
		char **x = realloc(*filesystems, items * sizeof(char *));

		if (!x)
			goto err;
		*filesystems = x;
	}
	name = strdup(name);
	(*filesystems)[n] = name;
	(*filesystems)[n + 1] = NULL;
	if (!name)
		goto err;
	return 0;
err:
	mnt_free_filesystems(*filesystems);
	return -ENOMEM;
}

static int get_filesystems(const char *filename, char ***filesystems, const char *pattern)
{
	int rc = 0;
	FILE *f;
	char line[129];

	f = fopen(filename, "r" UL_CLOEXECSTR);
	if (!f)
		return 1;

	DBG(UTILS, ul_debug("reading filesystems list from: %s", filename));

	while (fgets(line, sizeof(line), f)) {
		char name[sizeof(line)];

		if (*line == '#' || strncmp(line, "nodev", 5) == 0)
			continue;
		if (sscanf(line, " %128[^\n ]\n", name) != 1)
			continue;
		if (strcmp(name, "*") == 0) {
			rc = 1;
			break;		/* end of the /etc/filesystems */
		}
		if (pattern && !mnt_match_fstype(name, pattern))
			continue;
		rc = add_filesystem(filesystems, name);
		if (rc)
			break;
	}

	fclose(f);
	return rc;
}

/*
 * Always check the @filesystems pointer!
 *
 * man mount:
 *
 * ...mount will try to read the file /etc/filesystems, or, if that does not
 * exist, /proc/filesystems. All of the filesystem  types  listed  there  will
 * be tried,  except  for  those  that  are  labeled  "nodev"  (e.g.,  devpts,
 * proc  and  nfs).  If /etc/filesystems ends in a line with a single * only,
 * mount will read /proc/filesystems afterwards.
 */
int mnt_get_filesystems(char ***filesystems, const char *pattern)
{
	int rc;

	if (!filesystems)
		return -EINVAL;

	*filesystems = NULL;

	rc = get_filesystems(_PATH_FILESYSTEMS, filesystems, pattern);
	if (rc != 1)
		return rc;

	rc = get_filesystems(_PATH_PROC_FILESYSTEMS, filesystems, pattern);
	if (rc == 1 && *filesystems)
		rc = 0;			/* /proc/filesystems not found */

	return rc;
}

/*
 * Returns an allocated string with username or NULL.
 */
char *mnt_get_username(const uid_t uid)
{
        struct passwd pwd;
	struct passwd *res;
	char *buf, *username = NULL;

	buf = malloc(UL_GETPW_BUFSIZ);
	if (!buf)
		return NULL;

	if (!getpwuid_r(uid, &pwd, buf, UL_GETPW_BUFSIZ, &res) && res)
		username = strdup(pwd.pw_name);

	free(buf);
	return username;
}

int mnt_get_uid(const char *username, uid_t *uid)
{
	int rc = -1;
        struct passwd pwd;
	struct passwd *pw;
	char *buf;

	if (!username || !uid)
		return -EINVAL;

	buf = malloc(UL_GETPW_BUFSIZ);
	if (!buf)
		return -ENOMEM;

	if (!getpwnam_r(username, &pwd, buf, UL_GETPW_BUFSIZ, &pw) && pw) {
		*uid= pw->pw_uid;
		rc = 0;
	} else {
		DBG(UTILS, ul_debug(
			"cannot convert '%s' username to UID", username));
		rc = errno ? -errno : -EINVAL;
	}

	free(buf);
	return rc;
}

int mnt_get_gid(const char *groupname, gid_t *gid)
{
	int rc = -1;
        struct group grp;
	struct group *gr;
	char *buf;

	if (!groupname || !gid)
		return -EINVAL;

	buf = malloc(UL_GETPW_BUFSIZ);
	if (!buf)
		return -ENOMEM;

	if (!getgrnam_r(groupname, &grp, buf, UL_GETPW_BUFSIZ, &gr) && gr) {
		*gid= gr->gr_gid;
		rc = 0;
	} else {
		DBG(UTILS, ul_debug(
			"cannot convert '%s' groupname to GID", groupname));
		rc = errno ? -errno : -EINVAL;
	}

	free(buf);
	return rc;
}

int mnt_in_group(gid_t gid)
{
	int rc = 0, n, i;
	gid_t *grps = NULL;

	if (getgid() == gid)
		return 1;

	n = getgroups(0, NULL);
	if (n <= 0)
		goto done;

	grps = malloc(n * sizeof(*grps));
	if (!grps)
		goto done;

	if (getgroups(n, grps) == n) {
		for (i = 0; i < n; i++) {
			if (grps[i] == gid) {
				rc = 1;
				break;
			}
		}
	}
done:
	free(grps);
	return rc;
}

static int try_write(const char *filename)
{
	int fd;

	if (!filename)
		return -EINVAL;

	fd = open(filename, O_RDWR|O_CREAT|O_CLOEXEC,
			    S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH);
	if (fd >= 0) {
		close(fd);
		return 0;
	}
	return -errno;
}

/**
 * mnt_has_regular_mtab:
 * @mtab: returns path to mtab
 * @writable: returns 1 if the file is writable
 *
 * If the file does not exist and @writable argument is not NULL, then it will
 * try to create the file.
 *
 * Returns: 1 if /etc/mtab is a regular file, and 0 in case of error (check
 *          errno for more details).
 */
int mnt_has_regular_mtab(const char **mtab, int *writable)
{
	struct stat st;
	int rc;
	const char *filename = mtab && *mtab ? *mtab : mnt_get_mtab_path();

	if (writable)
		*writable = 0;
	if (mtab && !*mtab)
		*mtab = filename;

	DBG(UTILS, ul_debug("mtab: %s", filename));

	rc = lstat(filename, &st);

	if (rc == 0) {
		/* file exists */
		if (S_ISREG(st.st_mode)) {
			if (writable)
				*writable = !try_write(filename);
			DBG(UTILS, ul_debug("%s: writable", filename));
			return 1;
		}
		goto done;
	}

	/* try to create the file */
	if (writable) {
		*writable = !try_write(filename);
		if (*writable) {
			DBG(UTILS, ul_debug("%s: writable", filename));
			return 1;
		}
	}

done:
	DBG(UTILS, ul_debug("%s: irregular/non-writable", filename));
	return 0;
}

/*
 * Don't export this to libmount API -- utab is private library stuff.
 *
 * If the file does not exist and @writable argument is not NULL, then it will
 * try to create the directory (e.g. /run/mount) and the file.
 *
 * Returns: 1 if utab is a regular file, and 0 in case of
 *          error (check errno for more details).
 */
int mnt_has_regular_utab(const char **utab, int *writable)
{
	struct stat st;
	int rc;
	const char *filename = utab && *utab ? *utab : mnt_get_utab_path();

	if (writable)
		*writable = 0;
	if (utab && !*utab)
		*utab = filename;

	DBG(UTILS, ul_debug("utab: %s", filename));

	rc = lstat(filename, &st);

	if (rc == 0) {
		/* file exists */
		if (S_ISREG(st.st_mode)) {
			if (writable)
				*writable = !try_write(filename);
			return 1;
		}
		goto done;	/* it's not a regular file */
	}

	if (writable) {
		char *dirname = strdup(filename);

		if (!dirname)
			goto done;

		stripoff_last_component(dirname);	/* remove filename */

		rc = mkdir(dirname, S_IWUSR|
				    S_IRUSR|S_IRGRP|S_IROTH|
				    S_IXUSR|S_IXGRP|S_IXOTH);
		free(dirname);
		if (rc && errno != EEXIST)
			goto done;			/* probably EACCES */

		*writable = !try_write(filename);
		if (*writable)
			return 1;
	}
done:
	DBG(UTILS, ul_debug("%s: irregular/non-writable file", filename));
	return 0;
}

/**
 * mnt_get_swaps_path:
 *
 * Returns: path to /proc/swaps or $LIBMOUNT_SWAPS.
 */
const char *mnt_get_swaps_path(void)
{
	const char *p = safe_getenv("LIBMOUNT_SWAPS");
	return p ? : _PATH_PROC_SWAPS;
}

/**
 * mnt_get_fstab_path:
 *
 * Returns: path to /etc/fstab or $LIBMOUNT_FSTAB.
 */
const char *mnt_get_fstab_path(void)
{
	const char *p = safe_getenv("LIBMOUNT_FSTAB");
	return p ? : _PATH_MNTTAB;
}

/**
 * mnt_get_mtab_path:
 *
 * This function returns the *default* location of the mtab file. The result does
 * not have to be writable. See also mnt_has_regular_mtab().
 *
 * Returns: path to /etc/mtab or $LIBMOUNT_MTAB.
 */
const char *mnt_get_mtab_path(void)
{
	const char *p = safe_getenv("LIBMOUNT_MTAB");
	return p ? : _PATH_MOUNTED;
}

/*
 * Don't export this to libmount API -- utab is private library stuff.
 *
 * Returns: path to /run/mount/utab (or /dev/.mount/utab) or $LIBMOUNT_UTAB.
 */
const char *mnt_get_utab_path(void)
{
	struct stat st;
	const char *p = safe_getenv("LIBMOUNT_UTAB");

	if (p)
		return p;

	if (stat(MNT_RUNTIME_TOPDIR, &st) == 0)
		return MNT_PATH_UTAB;

	return MNT_PATH_UTAB_OLD;
}


/* returns file descriptor or -errno, @name returns a unique filename
 */
int mnt_open_uniq_filename(const char *filename, char **name)
{
	int rc, fd;
	char *n;
	mode_t oldmode;

	if (!filename)
		return -EINVAL;
	if (name)
		*name = NULL;

	rc = asprintf(&n, "%s.XXXXXX", filename);
	if (rc <= 0)
		return -errno;

	/* This is for very old glibc and for compatibility with Posix, which says
	 * nothing about mkstemp() mode. All sane glibc use secure mode (0600).
	 */
	oldmode = umask(S_IRGRP|S_IWGRP|S_IXGRP|
			S_IROTH|S_IWOTH|S_IXOTH);
	fd = mkostemp(n, O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC);
	if (fd < 0)
		fd = -errno;
	umask(oldmode);

	if (fd >= 0 && name)
		*name = n;
	else
		free(n);

	return fd;
}

/**
 * mnt_get_mountpoint:
 * @path: pathname
 *
 * This function finds the mountpoint that a given path resides in. @path
 * should be canonicalized. The returned pointer should be freed by the caller.
 *
 * WARNING: the function compares st_dev of the @path elements. This traditional
 * way maybe be insufficient on filesystems like Linux "overlay". See also
 * mnt_table_find_target().
 *
 * Returns: allocated string with the target of the mounted device or NULL on error
 */
char *mnt_get_mountpoint(const char *path)
{
	char *mnt;
	struct stat st;
	dev_t dir, base;

	if (!path)
		return NULL;

	mnt = strdup(path);
	if (!mnt)
		return NULL;
	if (*mnt == '/' && *(mnt + 1) == '\0')
		goto done;

	if (mnt_stat_mountpoint(mnt, &st))
		goto err;
	base = st.st_dev;

	do {
		char *p = stripoff_last_component(mnt);

		if (!p)
			break;
		if (mnt_stat_mountpoint(*mnt ? mnt : "/", &st))
			goto err;
		dir = st.st_dev;
		if (dir != base) {
			if (p > mnt)
				*(p - 1) = '/';
			goto done;
		}
		base = dir;
	} while (mnt && *(mnt + 1) != '\0');

	memcpy(mnt, "/", 2);
done:
	DBG(UTILS, ul_debug("%s mountpoint is %s", path, mnt));
	return mnt;
err:
	free(mnt);
	return NULL;
}

/*
 * Search for @name kernel command parametr.
 *
 * Returns newly allocated string with a parameter argument if the @name is
 * specified as "name=" or returns pointer to @name or returns NULL if not
 * found.  If it is specified more than once, we grab the last copy.
 *
 * For example cmdline: "aaa bbb=BBB ccc"
 *
 *	@name is "aaa"	--returns--> "aaa" (pointer to @name)
 *	@name is "bbb=" --returns--> "BBB" (allocated)
 *	@name is "foo"  --returns--> NULL
 *
 * Note: It is not really feasible to parse the command line exactly the same
 * as the kernel does since we don't know which options are valid.  We can use
 * the -- marker though and not walk past that.
 */
char *mnt_get_kernel_cmdline_option(const char *name)
{
	FILE *f;
	size_t len;
	int val = 0;
	char *p, *res = NULL, *mem = NULL;
	char buf[BUFSIZ];	/* see kernel include/asm-generic/setup.h: COMMAND_LINE_SIZE */
	const char *path = _PATH_PROC_CMDLINE;

	if (!name || !name[0])
		return NULL;

#ifdef TEST_PROGRAM
	path = getenv("LIBMOUNT_KERNEL_CMDLINE");
	if (!path)
		path = _PATH_PROC_CMDLINE;
#endif
	f = fopen(path, "r" UL_CLOEXECSTR);
	if (!f)
		return NULL;

	p = fgets(buf, sizeof(buf), f);
	fclose(f);

	if (!p || !*p || *p == '\n')
		return NULL;

	p = strstr(p, " -- ");
	if (p) {
		/* no more kernel args after this */
		*p = '\0';
	} else {
		len = strlen(buf);
		buf[len - 1] = '\0';	/* remove last '\n' */
	}

	len = strlen(name);
	if (name[len - 1] == '=')
		val = 1;

	for (p = buf; p && *p; p++) {
		if (!(p = strstr(p, name)))
			break;			/* not found the option */
		if (p != buf && !isblank(*(p - 1)))
			continue;		/* no space before the option */
		if (!val && *(p + len) != '\0' && !isblank(*(p + len)))
			continue;		/* no space after the option */
		if (val) {
			char *v = p + len;
			int end;

			while (*p && !isblank(*p))	/* jump to the end of the argument */
				p++;
			end = (*p == '\0');
			*p = '\0';
			free(mem);
			res = mem = strdup(v);
			if (end)
				break;
		} else
			res = (char *) name;	/* option without '=' */
		/* don't break -- keep scanning for more options */
	}

	return res;
}

#ifdef TEST_PROGRAM
int test_match_fstype(struct libmnt_test *ts, int argc, char *argv[])
{
	char *type = argv[1];
	char *pattern = argv[2];

	printf("%s\n", mnt_match_fstype(type, pattern) ? "MATCH" : "NOT-MATCH");
	return 0;
}

int test_match_options(struct libmnt_test *ts, int argc, char *argv[])
{
	char *optstr = argv[1];
	char *pattern = argv[2];

	printf("%s\n", mnt_match_options(optstr, pattern) ? "MATCH" : "NOT-MATCH");
	return 0;
}

int test_startswith(struct libmnt_test *ts, int argc, char *argv[])
{
	char *optstr = argv[1];
	char *pattern = argv[2];

	printf("%s\n", startswith(optstr, pattern) ? "YES" : "NOT");
	return 0;
}

int test_endswith(struct libmnt_test *ts, int argc, char *argv[])
{
	char *optstr = argv[1];
	char *pattern = argv[2];

	printf("%s\n", endswith(optstr, pattern) ? "YES" : "NOT");
	return 0;
}

int test_appendstr(struct libmnt_test *ts, int argc, char *argv[])
{
	char *str = strdup(argv[1]);
	const char *ap = argv[2];

	append_string(&str, ap);
	printf("new string: '%s'\n", str);

	free(str);
	return 0;
}

int test_mountpoint(struct libmnt_test *ts, int argc, char *argv[])
{
	char *path = canonicalize_path(argv[1]),
	     *mnt = path ? mnt_get_mountpoint(path) :  NULL;

	printf("%s: %s\n", argv[1], mnt ? : "unknown");
	free(mnt);
	free(path);
	return 0;
}

int test_filesystems(struct libmnt_test *ts, int argc, char *argv[])
{
	char **filesystems = NULL;
	int rc;

	rc = mnt_get_filesystems(&filesystems, argc ? argv[1] : NULL);
	if (!rc) {
		char **p;
		for (p = filesystems; *p; p++)
			printf("%s\n", *p);
		mnt_free_filesystems(filesystems);
	}
	return rc;
}

int test_chdir(struct libmnt_test *ts, int argc, char *argv[])
{
	int rc;
	char *path = canonicalize_path(argv[1]),
	     *last = NULL;

	if (!path)
		return -errno;

	rc = mnt_chdir_to_parent(path, &last);
	if (!rc) {
		printf("path='%s', abs='%s', last='%s'\n",
				argv[1], path, last);
	}
	free(path);
	free(last);
	return rc;
}

int test_kernel_cmdline(struct libmnt_test *ts, int argc, char *argv[])
{
	char *name = argv[1];
	char *res;

	res = mnt_get_kernel_cmdline_option(name);
	if (!res)
		printf("'%s' not found\n", name);
	else if (res == name)
		printf("'%s' found\n", name);
	else {
		printf("'%s' found, argument: '%s'\n", name, res);
		free(res);
	}

	return 0;
}

int test_mkdir(struct libmnt_test *ts, int argc, char *argv[])
{
	int rc;

	rc = mkdir_p(argv[1], S_IRWXU |
			 S_IRGRP | S_IXGRP |
			 S_IROTH | S_IXOTH);
	if (rc)
		printf("mkdir %s failed\n", argv[1]);
	return rc;
}

int test_statfs_type(struct libmnt_test *ts, int argc, char *argv[])
{
	struct statfs vfs;
	int rc;

	rc = statfs(argv[1], &vfs);
	if (rc)
		printf("%s: statfs failed: %m\n", argv[1]);
	else
		printf("%-30s: statfs type: %-12s [0x%lx]\n", argv[1],
				mnt_statfs_get_fstype(&vfs),
				(long) vfs.f_type);
	return rc;
}


int main(int argc, char *argv[])
{
	struct libmnt_test tss[] = {
	{ "--match-fstype",  test_match_fstype,    "<type> <pattern>     FS types matching" },
	{ "--match-options", test_match_options,   "<options> <pattern>  options matching" },
	{ "--filesystems",   test_filesystems,	   "[<pattern>] list /{etc,proc}/filesystems" },
	{ "--starts-with",   test_startswith,      "<string> <prefix>" },
	{ "--ends-with",     test_endswith,        "<string> <prefix>" },
	{ "--append-string", test_appendstr,       "<string> <appendix>" },
	{ "--mountpoint",    test_mountpoint,      "<path>" },
	{ "--cd-parent",     test_chdir,           "<path>" },
	{ "--kernel-cmdline",test_kernel_cmdline,  "<option> | <option>=" },
	{ "--mkdir",         test_mkdir,           "<path>" },
	{ "--statfs-type",   test_statfs_type,     "<path>" },

	{ NULL }
	};

	return mnt_run_test(tss, argc, argv);
}

#endif /* TEST_PROGRAM */
