/*
 * Phoenix-RTOS
 *
 * Tool to generate syspage based on plo scripts
 *
 * Copyright 2022 Phoenix Systems
 *
 * Author: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/queue.h>

#include "syspage.h"


#define ALIGN_ADDR(addr, align) (align ? ((addr + (align - 1)) & ~(align - 1)) : addr)
#define CAST_SYSPTR(type, ptr)  ((type)(sysgen_common.buff + ptr - (sysgen_common.pkernel + sysgen_common.offs)))

/* Reserve +1 for terminating NULL pointer in conformance to C standard */
#define SIZE_CMD_ARGV     (10 + 1)
#define SIZE_CMD_ARG_LINE 181


struct phfs_alias_t {
	char name[32];
	addr_t addr;
	size_t size;

	LIST_ENTRY(phfs_alias_t) ptrs;
};


typedef struct {
	const char *name;
	const int (*run)(int, char *[]);
} cmd_t;


struct {
	addr_t pkernel;
	off_t offs;
	size_t maxsz;

	syspage_t *syspage;
	uint8_t *buff;

	LIST_HEAD(plist_t, phfs_alias_t) aliases;
} sysgen_common;


extern int sysgen_cmdAlias(int argc, char *argv[]);
extern int sysgen_cmdMap(int argc, char *argv[]);
extern int sysgen_cmdApp(int argc, char *argv[]);
extern int sysgen_cmdConsole(int argc, char *argv[]);


static const cmd_t cmds[] = {
	{ .name = "alias", .run = sysgen_cmdAlias },
	{ .name = "map", .run = sysgen_cmdMap },
	{ .name = "app", .run = sysgen_cmdApp },
	{ .name = "console", .run = sysgen_cmdConsole }
};

enum { flagSyspageExec = 0x01 };


static sysptr_t sysgen_buffAlloc(size_t sz)
{
	sysptr_t ptr;
	size_t newSz = ALIGN_ADDR(sysgen_common.syspage->size + sz, sizeof(long long));

	if (newSz >= sysgen_common.maxsz) {
		fprintf(stderr, "Cannot allocate size 0x%x; current buffer size 0x%x\n", (unsigned int)sz, sysgen_common.syspage->size);
		return 0;
	}

	ptr = sysgen_common.syspage->size + sysgen_common.pkernel + sysgen_common.offs;
	sysgen_common.syspage->size = newSz;

	return ptr;
}


static struct phfs_alias_t *sysgen_aliasFind(const char *name)
{
	struct phfs_alias_t *alias;

	LIST_FOREACH(alias, &sysgen_common.aliases, ptrs) {
		if (strcmp(alias->name, name) == 0)
			return alias;
	}

	return NULL;
}


int sysgen_cmdAlias(int argc, char *argv[])
{
	char *end;
	unsigned int i;
	addr_t addr = 0;
	size_t size = 0;
	struct phfs_alias_t *alias;

	if (argc != 4) {
		fprintf(stderr, "\n%s: Wrong argument count", argv[0]);
		return -1;
	}

	for (i = 2; i < 4; ++i) {
		if (i == 2)
			addr = strtoul(argv[i], &end, 0);
		else if (i == 3)
			size = strtoul(argv[i], &end, 0);

		if (*end) {
			fprintf(stderr, "\n%s: Wrong arguments", argv[0]);
			return -1;
		}
	}

	alias = malloc(sizeof(struct phfs_alias_t));
	if (alias == NULL)
		return -1;

	strncpy(alias->name, argv[1], sizeof(alias->name) - 1);
	alias->name[sizeof(alias->name) - 1] = '\0';

	/* Update max image size */
	if (sysgen_common.syspage->hs.imgsz < (addr + size))
		sysgen_common.syspage->hs.imgsz = addr + size;

	alias->addr = addr + sysgen_common.pkernel;
	alias->size = size;

	LIST_INSERT_HEAD(&sysgen_common.aliases, alias, ptrs);

	return 0;
}


static int sysgen_mapOverlapping(const char *name, addr_t start, addr_t end)
{
	sysptr_t ptr;
	const syspage_map_t *map;

	if (sysgen_common.syspage->maps != 0) {
		ptr = sysgen_common.syspage->maps;
		do {
			map = CAST_SYSPTR(syspage_map_t *, ptr);
			if (((map->start < end) && (map->end > start)) ||
					(strcmp(name, CAST_SYSPTR(char *, map->name)) == 0))
				return -1;

		} while ((ptr = map->next) != sysgen_common.syspage->maps);
	}

	return 0;
}


static int sysgen_mapNameResolve(const char *name, uint8_t *id)
{
	sysptr_t ptr;
	const syspage_map_t *map;

	if (sysgen_common.syspage->maps != 0) {
		ptr = sysgen_common.syspage->maps;
		do {
			map = CAST_SYSPTR(syspage_map_t *, ptr);

			if (strcmp(name, CAST_SYSPTR(char *, map->name)) == 0) {
				*id = map->id;
				return 0;
			}

		} while ((ptr = map->next) != sysgen_common.syspage->maps);
	}

	return -1;
}


static int sysgen_strAttr2ui(const char *str, unsigned int *attr)
{
	int i;

	*attr = 0;
	for (i = 0; str[i]; ++i) {
		switch (str[i]) {
			case 'r':
				*attr |= mAttrRead;
				break;
			case 'w':
				*attr |= mAttrWrite;
				break;
			case 'x':
				*attr |= mAttrExec;
				break;
			case 's':
				*attr |= mAttrShareable;
				break;
			case 'c':
				*attr |= mAttrCacheable;
				break;
			case 'b':
				*attr |= mAttrBufferable;
				break;
			default:
				fprintf(stderr, "\nsysgen: Wrong attribute - '%c'", str[i]);
				return -1;
		}
	}

	return 0;
}


static int sysgen_mapAdd(const char *mapName, addr_t start, addr_t end, unsigned int attr)
{
	size_t len;
	char *name;
	sysptr_t ptr;
	syspage_map_t *map, *headMap;

	ptr = sysgen_buffAlloc(sizeof(syspage_map_t));
	if (ptr == 0)
		return -1;

	map = CAST_SYSPTR(syspage_map_t *, ptr);

	len = strlen(mapName);
	map->name = sysgen_buffAlloc(len + 1);
	if (map->name == 0)
		return -1;

	name = CAST_SYSPTR(char *, map->name);
	memcpy(name, mapName, len + 1);

	map->entries = 0;
	map->start = start;
	map->end = end;
	map->attr = attr;

	if (sysgen_common.syspage->maps == 0) {
		map->next = ptr;
		map->prev = ptr;
		sysgen_common.syspage->maps = ptr;

		map->id = 0;
	}
	else {
		headMap = CAST_SYSPTR(syspage_map_t *, sysgen_common.syspage->maps);

		map->prev = headMap->prev;
		CAST_SYSPTR(syspage_map_t *, headMap->prev)->next = ptr;
		map->next = sysgen_common.syspage->maps;
		headMap->prev = ptr;

		map->id = CAST_SYSPTR(syspage_map_t *, map->prev)->id + 1;
	}

	return 0;
}


int sysgen_cmdMap(int argc, char *argv[])
{
	int res;
	char *endptr;
	addr_t start, end;
	unsigned int attr;

	if (argc != 5) {
		fprintf(stderr, "\n%s: Wrong argument count", argv[0]);
		return -1;
	}

	start = strtoul(argv[2], &endptr, 0);
	if (*endptr) {
		fprintf(stderr, "\n%s: Wrong arguments", argv[0]);
		return -1;
	}

	end = strtoul(argv[3], &endptr, 0);
	if (*endptr) {
		fprintf(stderr, "\n%s: Wrong arguments", argv[0]);
		return -1;
	}
	res = sysgen_strAttr2ui(argv[4], &attr);
	if (res < 0)
		return res;

	/* Check whether map's name exists or map overlaps with other maps */
	res = sysgen_mapOverlapping(argv[1], start, end);
	if (res < 0)
		return res;


	return sysgen_mapAdd(argv[1], start, end, attr);
}


static int sysgen_mapsAdd2Prog(uint8_t *mapIDs, size_t nb, const char *mapNames)
{
	int res;
	uint8_t id = 0;
	unsigned int i;

	for (i = 0; i < nb; ++i) {
		if ((res = sysgen_mapNameResolve(mapNames, &id)) < 0) {
			fprintf(stderr, "\nCan't add map %s", mapNames);
			return res;
		}

		mapIDs[i] = id;
		mapNames += strlen(mapNames) + 1; /* name + '\0' */
	}

	return 0;
}


static size_t sysgen_mapsParse(char *maps, char sep)
{
	size_t nb = 0;

	while (*maps != '\0') {
		if (*maps == sep) {
			*maps = '\0';
			++nb;
		}
		maps++;
	}

	return ++nb;
}


static int sysgen_appAdd(const char *name, char *imaps, char *dmaps, const char *appArgv, uint32_t flags)
{
	int res;
	char *argv;
	sysptr_t ptr;
	struct phfs_alias_t *alias;
	syspage_prog_t *prog, *headProg;

	size_t dmapSz, imapSz, len, argvSz;
	const uint32_t isExec = (flags & flagSyspageExec) != 0;

	alias = sysgen_aliasFind(name);
	if (alias == NULL)
		return -1;

	/* First instance in imap is a map for the instructions */
	imapSz = sysgen_mapsParse(imaps, ';');
	dmapSz = sysgen_mapsParse(dmaps, ';');

	len = strlen(appArgv);
	argvSz = isExec + len + 1; /* [X] + argv + '\0' */

	ptr = sysgen_buffAlloc(sizeof(syspage_prog_t));
	if (ptr == 0)
		return -1;

	prog = CAST_SYSPTR(syspage_prog_t *, ptr);
	prog->dmaps = sysgen_buffAlloc(sizeof(uint8_t) * dmapSz);
	prog->imaps = sysgen_buffAlloc(sizeof(uint8_t) * imapSz);
	prog->argv = sysgen_buffAlloc(sizeof(uint8_t) * argvSz);

	if ((prog->dmaps == 0) || (prog->imaps == 0) || (prog->argv == 0))
		return -1;

	prog->imapSz = imapSz;
	prog->dmapSz = dmapSz;
	prog->start = alias->addr;
	prog->end = alias->addr + alias->size;

	argv = CAST_SYSPTR(char *, prog->argv);
	if (isExec)
		argv[0] = 'X';

	memcpy(argv + isExec, appArgv, len);
	argv[argvSz - 1] = '\0';

	if ((res = sysgen_mapsAdd2Prog(CAST_SYSPTR(uint8_t *, prog->imaps), imapSz, imaps)) < 0 ||
		(res = sysgen_mapsAdd2Prog(CAST_SYSPTR(uint8_t *, prog->dmaps), dmapSz, dmaps)) < 0)
		return res;

	if (sysgen_common.syspage->progs == 0) {
		prog->next = ptr;
		prog->prev = ptr;
		sysgen_common.syspage->progs = ptr;
	}
	else {
		headProg = CAST_SYSPTR(syspage_prog_t *, sysgen_common.syspage->progs);

		prog->prev = headProg->prev;
		CAST_SYSPTR(syspage_prog_t *, headProg->prev)->next = ptr;
		prog->next = sysgen_common.syspage->progs;
		headProg->prev = ptr;
	}

	return 0;
}


int sysgen_cmdApp(int argc, char *argv[])
{
	size_t pos;
	int argvID = 0;

	char *imaps, *dmaps;
	unsigned int flags = 0;

	const char *appArgv;
	char name[SIZE_CMD_ARG_LINE];

	if (argc < 5 || argc > 6) {
		fprintf(stderr, "\n%s: Wrong argument count", argv[0]);
		return -1;
	}

	/* ARG_0: command name */

	/* ARG_1: alias to device - it will be checked in phfs_open */

	/* ARG_2: optional flags */
	argvID = 2;
	if (argv[argvID][0] == '-') {
		if ((argv[argvID][1] | 0x20) == 'x' && argv[argvID][2] == '\0') {
			flags |= flagSyspageExec;
			argvID++;
		}
		else {
			fprintf(stderr, "\n%s: Wrong arguments", argv[0]);
			return -1;
		}
	}

	if (argvID != (argc - 3)) {
		fprintf(stderr, "\n%s: Invalid arg, 'dmap' is not declared", argv[0]);
		return -1;
	}

	/* ARG_3: name + argv */
	appArgv = argv[argvID];
	for (pos = 0; appArgv[pos]; pos++) {
		if (appArgv[pos] == ';')
			break;
	}
	memcpy(name, appArgv, pos);
	name[pos] = '\0';

	/* ARG_4: maps for instructions */
	imaps = argv[++argvID];

	/* ARG_5: maps for data */
	dmaps = argv[++argvID];

	return sysgen_appAdd(name, imaps, dmaps, appArgv, flags);
}


int sysgen_cmdConsole(int argc, char *argv[])
{
	char *endptr;
	unsigned int minor;

	if (argc != 2) {
		fprintf(stderr, "\n%s: Wrong argument count", argv[0]);
		return -EINVAL;
	}

	strtoul(argv[1], &endptr, 0);
	if (*endptr != '.') {
		fprintf(stderr, "\nWrong major value: %s", argv[1]);
		return -EINVAL;
	}

	minor = strtoul(++endptr, &endptr, 0);
	if (*endptr != '\0') {
		fprintf(stderr, "\nWrong minor value: %s", argv[1]);
		return -EINVAL;
	}

	sysgen_common.syspage->console = minor;

	return 0;
}


static int sysgen_parseArgLine(const char *lines, char *buf, size_t bufsz, char *argv[], size_t argvsz)
{
	char *const end = buf + bufsz;
	int argc = 0;

	while (*lines != '\0') {
		if (isspace(*lines)) {
			if (isblank(*lines++))
				continue;
			else
				break;
		}

		/* Argument count and one NULL pointer */
		if (argc + 1 >= argvsz) {
			fprintf(stderr, "\nToo many arguments");
			return -EINVAL;
		}

		argv[argc++] = buf;

		while (isgraph(*lines) && buf < end)
			*buf++ = *lines++;

		if (buf >= end) {
			fprintf(stderr, "\nCommand buffer too small");
			return -ENOMEM;
		}

		*buf++ = '\0';
	}

	argv[argc] = NULL;

	return argc;
}


static int sysgen_parseScript(const char *fname)
{
	FILE *file;
	size_t len = 0;
	char *line = NULL;
	int sz, argc, i, ret;
	char *argv[SIZE_CMD_ARGV];
	char argline[SIZE_CMD_ARG_LINE];

	file = fopen(fname, "r");
	if (file == NULL) {
		fprintf(stderr, "Cannot open file %s\n", fname);
		return -1;
	}

	while ((sz = getline(&line, &len, file)) != -1) {
		/* EOF */
		if (line == NULL || *line == '\0') {
			free(line);
			fclose(file);
			return 0;
		}

		argc = sysgen_parseArgLine(line, argline, SIZE_CMD_ARG_LINE, argv, SIZE_CMD_ARGV);
		/* skip empty lines */
		if (argc == 0)
			continue;

		/* error */
		if (argc < 0) {
			free(line);
			fclose(file);
			return argc;
		}

		/* Find command and launch associated function */
		for (i = 0; i < sizeof(cmds) / sizeof(cmd_t); ++i) {
			if (strcmp(argv[0], cmds[i].name) != 0)
				continue;

			if ((ret = cmds[i].run(argc, argv)) < 0) {
				fprintf(stderr, "Failed %s\n", argv[0]);
				free(line);
				fclose(file);
				return ret;
			}

			break;
		}
	}

	free(line);
	fclose(file);

	return 0;
}


static int sysgen_addSyspage2Img(const char *imgName)
{
	int res;
	size_t sz;
	FILE *img;

	img = fopen(imgName, "rb+");
	if (img == NULL)
		return -1;

	res = fseek(img, sysgen_common.offs, SEEK_SET);
	if (res < 0) {
		fclose(img);
		return -1;
	}

	sz = fwrite(sysgen_common.buff, sizeof(uint8_t), sysgen_common.syspage->size, img);
	if (sz < sysgen_common.syspage->size) {
		fprintf(stderr, "Cannot write binary syspage into %s\n", imgName);
		fclose(img);
		return -1;
	}

	fclose(img);

	return 0;
}


static void sysgen_cleanup(void)
{
	struct phfs_alias_t *alias;

	while (!LIST_EMPTY(&sysgen_common.aliases)) {
		alias = LIST_FIRST(&sysgen_common.aliases);
		LIST_REMOVE(alias, ptrs);
		free(alias);
	}

	free(sysgen_common.buff);
}


static void sysgen_dump(void)
{
	sysptr_t ptr;
	const syspage_prog_t *prog;

	printf("\n\tSyspage:\n");
	printf("\tImage size: 0x%08x\n", sysgen_common.syspage->hs.imgsz);
	printf("\tSyspage size: 0x%08x\n", sysgen_common.syspage->size);
	printf("\tKernel physical address: 0x%08x\n", sysgen_common.syspage->pkernel);
	printf("\tConsole: 0x%02x\n", sysgen_common.syspage->console);
	printf("\tPrograms:\n");
	if (sysgen_common.syspage->progs != 0) {
		ptr = sysgen_common.syspage->progs;
		do {
			prog = CAST_SYSPTR(syspage_prog_t *, ptr);
			printf("\t\t%s\n", CAST_SYSPTR(char *, prog->argv));
		} while ((ptr = prog->next) != sysgen_common.syspage->progs);
	}
	else {
		printf("\t\tnot defined\n");
	}
}


static void sysgen_help(const char *prog)
{
	printf("Usage: %s to add syspage to image\n", prog);
	printf("Obligatory arguments:\n");
	printf("\t-s <pimg:offs:sz>   - syspage properties\n");
	printf("\t    pimg - beginning physical address of the target image\n");
	printf("\t    offs - syspage's offset in the target image\n");
	printf("\t    sz   - max syspage's size\n");
	printf("\t-p <path>           - path to preinit script\n");
	printf("\t-u <path>           - path to user script\n");
	printf("\t-i <path>           - path to image \n");
	printf("Options:\n");
	printf("\t-h                  - print help message\n");
}


int main(int argc, char *argv[])
{
	int opt;
	char *endptr;
	const char *preinitScript, *userScript, *binName, *imgName;

	preinitScript = userScript = binName = imgName = NULL;

	if (argc <= 1) {
		sysgen_help(argv[0]);
		return EXIT_FAILURE;
	}

	while ((opt = getopt(argc, argv, "s:p:u:i:h")) != -1) {
		switch (opt) {
			case 's':
				sysgen_common.pkernel = strtoul(optarg, &endptr, 0);
				if (*endptr != ':') {
					fprintf(stderr, "Wrong physical image address %s\n", optarg);
					return EXIT_FAILURE;
				}

				sysgen_common.offs = strtoul(endptr + 1, &endptr, 0);
				if (*endptr != ':') {
					fprintf(stderr, "Wrong syspage offset %s\n", optarg);
					return EXIT_FAILURE;
				}

				sysgen_common.maxsz = strtoul(endptr + 1, &endptr, 0);
				if (*endptr != '\0') {
					fprintf(stderr, "Wrong syspage size %s\n", optarg);
					return EXIT_FAILURE;
				}
				break;

			case 'p':
				preinitScript = optarg;
				break;

			case 'u':
				userScript = optarg;
				break;

			case 'i':
				imgName = optarg;
				break;

			case 'h':
				sysgen_help(argv[0]);
				return EXIT_SUCCESS;

			default:
				return EXIT_FAILURE;
		}
	}


	if (preinitScript == NULL || userScript == NULL || imgName == NULL || sysgen_common.maxsz == 0) {
		fprintf(stderr, "Missing obligatory arguments\n");
		sysgen_help(argv[0]);
		return EXIT_FAILURE;
	}


	sysgen_common.buff = calloc(sysgen_common.maxsz, sizeof(uint8_t));
	if (sysgen_common.buff == NULL)
		return EXIT_FAILURE;

	sysgen_common.syspage = (syspage_t *)sysgen_common.buff;
	sysgen_common.syspage->size = ALIGN_ADDR(sizeof(syspage_t), sizeof(long long));
	sysgen_common.syspage->pkernel = sysgen_common.pkernel;
	LIST_INIT(&sysgen_common.aliases);

	/* Parse preinit script with map and console commands */
	if (sysgen_parseScript(preinitScript) < 0) {
		fprintf(stderr, "Cannot parse preinit script: %s\n", preinitScript);
		sysgen_cleanup();
		return EXIT_FAILURE;
	}

	/* Parse user script descring programs in the syspage */
	if (sysgen_parseScript(userScript) < 0) {
		fprintf(stderr, "Cannot parse user script: %s\n", userScript);
		sysgen_cleanup();
		return EXIT_FAILURE;
	}

	/* Based on scripts write binary syspage directly to the image */
	if (sysgen_addSyspage2Img(imgName) < 0) {
		fprintf(stderr, "Cannot write binary syspage to kernel image: %s\n", imgName);
		sysgen_cleanup();
		return EXIT_FAILURE;
	}
	printf("Syspage is written to image: %s at offset 0x%lx\n", imgName, sysgen_common.offs);

	sysgen_dump();
	sysgen_cleanup();

	return EXIT_SUCCESS;
}
