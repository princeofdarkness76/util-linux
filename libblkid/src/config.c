/*
 * config.c - blkid.conf routines
 *
 * Copyright (C) 2009 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <stdint.h>
#include <stdarg.h>

#include "blkidP.h"
#include "env.h"
#include "strv.h"

static int parse_evaluate(struct blkid_config *conf, char *s)
{
	DBG(CONFIG, ul_debug("parse EVALUATE='%s'", s));
	while(s && *s) {
		char *sep;

		if (conf->nevals >= __BLKID_EVAL_LAST)
			goto err;
		sep = strchr(s, ',');
		if (sep)
			*sep = '\0';
		if (strcmp(s, "udev") == 0)
			conf->eval[conf->nevals] = BLKID_EVAL_UDEV;
		else if (strcmp(s, "scan") == 0)
			conf->eval[conf->nevals] = BLKID_EVAL_SCAN;
		else
			goto err;
		conf->nevals++;
		if (sep)
			s = sep + 1;
		else
			break;
	}
	return 0;
err:
	DBG(CONFIG, ul_debug(
		"config file: unknown evaluation method '%s'.", s));
	return -1;
}

static int parse_probeoff(struct blkid_config *conf, char *s)
{
	DBG(CONFIG, ul_debug("parse PROBE_OFF='%s'", s));
	conf->probeoff = strv_split(s, ",");
	return 0;
}

static int parse_next(FILE *fd, struct blkid_config *conf)
{
	char buf[BUFSIZ];
	char *s;

	/* read the next non-blank non-comment line */
	do {
		if (fgets (buf, sizeof(buf), fd) == NULL)
			return feof(fd) ? 0 : -1;
		s = strchr (buf, '\n');
		if (!s) {
			/* Missing final newline?  Otherwise extremely */
			/* long line - assume file was corrupted */
			if (feof(fd))
				s = strchr (buf, '\0');
			else {
				DBG(CONFIG, ul_debug(
					"config file: missing newline at line '%s'.",
					buf));
				return -1;
			}
		}
		*s = '\0';
		if (--s >= buf && *s == '\r')
			*s = '\0';

		s = buf;
		while (*s == ' ' || *s == '\t')		/* skip space */
			s++;

	} while (*s == '\0' || *s == '#');

	if (!strncmp(s, "SEND_UEVENT=", 12)) {
		s += 13;
		if (*s && !strcasecmp(s, "yes"))
			conf->uevent = TRUE;
		else if (*s)
			conf->uevent = FALSE;
	} else if (!strncmp(s, "CACHE_FILE=", 11)) {
		s += 11;
		if (*s)
			conf->cachefile = strdup(s);
	} else if (!strncmp(s, "EVALUATE=", 9)) {
		s += 9;
		if (*s && parse_evaluate(conf, s) == -1)
			return -1;
	} else if (!strncmp(s, "PROBE_OFF=", 10)) {
		s += 10;
		if (*s && parse_probeoff(conf, s) == -1)
			return -1;
	} else {
		DBG(CONFIG, ul_debug(
			"config file: unknown option '%s'.", s));
		return -1;
	}
	return 0;
}

/* return real config data or built-in default, use blkid_unref_config() for result */
struct blkid_config *blkid_read_config(void)
{
	struct blkid_config *conf;
	char *filename;
	FILE *f;

	filename = safe_getenv("BLKID_CONF");
	if (!filename)
		filename = BLKID_CONFIG_FILE;

	conf = (struct blkid_config *) calloc(1, sizeof(*conf));
	if (!conf)
		return NULL;
	conf->uevent = -1;
	conf->refcount = 1;

	DBG(CONFIG, ul_debug("reading config file: %s.", filename));

	f = fopen(filename, "r" UL_CLOEXECSTR);
	if (!f) {
		DBG(CONFIG, ul_debug("%s: does not exist, using built-in default", filename));
		goto dflt;
	}
	while (!feof(f)) {
		if (parse_next(f, conf)) {
			DBG(CONFIG, ul_debug("%s: parse error", filename));
			goto err;
		}
	}
dflt:
	if (!conf->nevals) {
		conf->eval[0] = BLKID_EVAL_UDEV;
		conf->eval[1] = BLKID_EVAL_SCAN;
		conf->nevals = 2;
	}
	if (!conf->cachefile)
		conf->cachefile = strdup(blkid_get_default_cache_filename());
	if (conf->uevent == -1)
		conf->uevent = TRUE;
	if (f)
		fclose(f);
	return conf;
err:
	fclose(f);
	blkid_unref_config(conf);
	return NULL;
}

/* Use this rather than blkid_read_config() if you already have cache. Caller
 * has to call blkid_unref_config() for result!
 */
struct blkid_config *blkid_get_config(blkid_cache cache)
{
	if (!cache->conf) {
		cache->conf = blkid_read_config();
		if (!cache->conf)
			return NULL;
	}
	blkid_ref_config(cache->conf);
	return cache->conf;
}


void blkid_ref_config(struct blkid_config *conf)
{
	if (conf)
		conf->refcount++;
}

void blkid_unref_config(struct blkid_config *conf)
{
	if (conf) {
		conf->refcount--;
		if (conf->refcount <= 0) {
			DBG(CONFIG, ul_debug("freeing"));
			free(conf->cachefile);
			strv_free(conf->probeoff);
			free(conf);
		}
	}
}

#ifdef TEST_PROGRAM
/*
 * usage: tst_config
 */
int main(int argc, char *argv[])
{
	int i;
	struct blkid_config *conf;

	blkid_init_debug(BLKID_DEBUG_ALL);

	conf = blkid_read_config();
	if (!conf)
		return EXIT_FAILURE;

	printf("EVALUATE:    ");
	for (i = 0; i < conf->nevals; i++)
		printf("%s ", conf->eval[i] == BLKID_EVAL_UDEV ? "udev" : "scan");
	printf("\n");

	printf("SEND UEVENT: %s\n", conf->uevent ? "TRUE" : "FALSE");
	printf("CACHE_FILE:  %s\n", conf->cachefile);

	blkid_unref_config(conf);
	return EXIT_SUCCESS;
}
#endif
