/*
 *  kexecboot - A kexec based bootloader
 *
 *  Copyright (c) 2008-2011 Yuri Bushmelev <jay4mail@gmail.com>
 *  Copyright (c) 2008 Thomas Kunze <thommycheck@gmx.de>
 *
 *  small parts:
 *  Copyright (c) 2006 Matthew Allum <mallum@o-hand.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <sys/mount.h>
#include <ctype.h>
#include <errno.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "config.h"
#include "util.h"
#include "cfgparser.h"
#include "devicescan.h"
#include "evdevs.h"
#include "menu.h"
#include "kexecboot.h"

#ifdef USE_FBMENU
#include "gui.h"
#endif

#ifdef USE_TEXTUI
#include "tui.h"
#endif

/* Don't re-create devices when executing on host */
#ifdef USE_HOST_DEBUG
#undef USE_DEVICES_RECREATING
#endif

#define PREPEND_MOUNTPATH(string) MOUNTPOINT""string

#define MAX_LOAD_ARGV_NR	(12 + 1)
#define MAX_EXEC_ARGV_NR	(3 + 1)
#define MAX_ARG_LEN		256

/* NULL-terminated array of kernel search paths
 * First item should be filled with machine-dependent path */
char *default_kernels[] = {
#ifdef USE_ZIMAGE
	PREPEND_MOUNTPATH("/boot/zImage"),
	PREPEND_MOUNTPATH("/zImage"),
#endif
#ifdef USE_UIMAGE
	PREPEND_MOUNTPATH("/boot/uImage"),
	PREPEND_MOUNTPATH("/uImage"),
#endif
	NULL
};

/* Init mode flag */
int initmode = 0;

/* Contexts available - menu and textview */
typedef enum {
	KX_CTX_MENU,
	KX_CTX_TEXTVIEW,
} kx_context;

/* Common parameters */
struct params_t {
	struct cfgdata_t *cfg;
	struct bootconf_t *bootcfg;
	kx_menu *menu;
	kx_context context;
#ifdef USE_FBMENU
	struct gui_t *gui;
#endif
#ifdef USE_TEXTUI
	kx_tui *tui;
#endif
};

static char *kxb_ttydev = NULL;
static int kxb_echo_state = 0;

static void atexit_restore_terminal(void)
{
	setup_terminal(kxb_ttydev, &kxb_echo_state, 0);
}

static void add_cmd_option(char **load_argv,
			   const char *start,
			   char *path, int *idx)
{
	const char mount_point[] = MOUNTPOINT;
	char buf[512];
	char *arg;
	int len;

	if (!path)
		return;

	/* allocate space */
	if (start)
		len = sizeof(start) + strlen(path) + 1;
	else
		len = strlen(path) + 1;
	len += sizeof(mount_point) + sizeof(buf);

	arg = (char *)malloc(len);
	if (NULL == arg) {
		perror("Can't allocate memory for arg");

		return;
	}

	memset(arg, '\0', len);
	if (start)
		strcpy(arg, start);
	strcat(arg, path);

	if ((len = readlink(path, buf, sizeof(buf) - 1)) != -1) {
		buf[len] = '\0';
		/* Fix absolute symlinks: prepend MOUNTPOINT */
		if (buf[0] == '/') {
			memset(arg, '\0', len);
			if (start)
				strcpy(arg, start);
			strcat(arg, mount_point);
			strcat(arg, buf);
		}
	}
	load_argv[(*idx)++] = arg;
}

/*
 * Add extra tags if UBI device is found.
 *
 * Sample commandline required to boot ubifs:
 * root=ubi0_0 ubi.mtd=2 rootfstype=ubifs
 */
static int check_for_ubi(struct boot_item_t *item,
			  char *cmdline_arg,
			  char *mount_dev,
			  char *mount_fstype,
			  const char *str_ubirootdev,
			  const char *str_ubimtd,
			  char *str_mtd_id,
			  const char *str_ubimtd_off)
{
	int u;

	if (!strncmp(item->fstype,"ubi",3)) {

		/* mtd id [0-15] - one or two digits */
		if(isdigit(atoi(item->device+strlen(item->device)-2))) {
			strcpy(str_mtd_id, item->device+strlen(item->device)-2);
			strcat(str_mtd_id, item->device+strlen(item->device)-1);
		} else {
			strcpy(str_mtd_id, item->device+strlen(item->device)-1);
		}
		/* get corresponding ubi dev to mount */
		u = find_attached_ubi_device(str_mtd_id);

		sprintf(mount_dev, "/dev/ubi%d", u);
		/* FIXME: first volume is hardcoded */
		strcat(mount_dev, "_0");

		/* HARDCODED: we assume it's ubifs */
		strcpy(mount_fstype,"ubifs");

		/* extra cmdline tags when we detect ubi */
		strcat(cmdline_arg, str_ubirootdev);
		/* FIXME: first volume is hardcoded */
		strcat(cmdline_arg, "_0");

		strcat(cmdline_arg, str_ubimtd);
		strcat(cmdline_arg, str_mtd_id);
#ifdef UBI_VID_HDR_OFFSET
		strcat(cmdline_arg, str_ubimtd_off);
#endif
		return 1;
	} else {
		return 0;
	}
}

void pre_start_kernel(struct params_t *params, int choice)
{
	const char mount_point[] = MOUNTPOINT;
	const char mount_point_dev[] = MOUNTPOINT "/dev";
	const char *exec_cmdline_path = NULL;
	char **load_argv;
	char stdout_str[COMMAND_LINE_SIZE] = {0};
	int stdout_str_len = sizeof(stdout_str);
	char exec_str[COMMAND_LINE_SIZE] = {0};

	char mount_dev[16];
	char mount_fstype[16];

	struct cfgdata_t cfgdata;

	/* empty environment */
	char *const _envp[] = { NULL };
	char *const envp[] = { NULL };

	struct boot_item_t *item;
	int n;
	char *p;

	item = params->bootcfg->list[choice];

	if (item->exec_cmdline) {
		if (item->device) {
			/* default device to mount */
			strcpy(mount_dev, item->device);

			if (item->fstype) {
				/* default fstype to mount */
				strcpy(mount_fstype, item->fstype);
			}
		}

		/* Mount boot device */
		if ( -1 == mount(mount_dev, mount_point, mount_fstype,
				MS_RDONLY, NULL) ) {
			perror("Can't mount boot device");
			exit(-1);
		}

		strcpy(mount_dev, "/dev");

		/* Bind /dev to MOUNTPOINT/dev */
		if ( -1 == mount(mount_dev, mount_point_dev, mount_fstype,
				MS_BIND, NULL) ) {
			perror("Can't mount boot device");
			exit(-1);
		}

		strcpy(exec_str, item->exec_cmdline);

		load_argv = buildargv(exec_str);
		exec_cmdline_path = load_argv[0];

		log_msg(lg, "Executing Append Kernel cmdline: %s", exec_cmdline_path);

		n = fexecwstr(exec_cmdline_path, (char *const *)load_argv,
			envp, stdout_str, stdout_str_len,
			MOUNTPOINT);
		if (-1 == n) {
			perror("exec_cmdline can't load");
			exit(-1);
		}

		init_cfgdata(&cfgdata);

		cfg_section_new(&cfgdata);

		/* Parse stdout_str and update item */
		parse_exec_cmdline(&cfgdata, stdout_str);

		/* Update item */
		if (cfgdata.current->dtbpath) {
			item->dtbpath = cfgdata.current->dtbpath;
		}
		if (cfgdata.current->cmdline_append) {
			p = malloc(strlen(item->cmdline_append) + strlen(cfgdata.current->cmdline_append) + 1);
			if (NULL == p)
				perror("Can't allocate memory to store cmdline_append");

			strcpy(p, item->cmdline_append);
			strcat(p, " ");
			strcat(p, cfgdata.current->cmdline_append);

			dispose(item->cmdline_append);
			item->cmdline_append = p;
		}

		destroy_cfgdata(&cfgdata);

		umount(mount_point_dev);
		umount(mount_point);

		freeargv(load_argv);
	}
}

void start_kernel(struct params_t *params, int choice)
{
	int n, idx, u;
	struct stat sinfo;
	struct boot_item_t *item;

	char mount_dev[16];
	char mount_fstype[16];
	char str_mtd_id[3];

	/* empty environment */
	char *const envp[] = { NULL };

	/* options set during configuration */
	const char mount_point[] = MOUNTPOINT;

	/* for --command-line */
	char *cmdline_arg = NULL;
	const char str_cmdline_start[] = "--command-line=";
#ifdef UBI_VID_HDR_OFFSET
	const char str_ubimtd_off[] = "," UBI_VID_HDR_OFFSET;
#else
	const char str_ubimtd_off[] = "";
#endif

	/* selected cmdline tags read from host kernel cmdline */
	const char str_mtdparts[] = " mtdparts=";
	const char str_fbcon[] = " fbcon=";

	/* initialize args */
	char **load_argv, **exec_argv;

	load_argv = calloc(MAX_LOAD_ARGV_NR, sizeof(*load_argv));
	if (!load_argv)
		return;

	exec_argv = calloc(MAX_EXEC_ARGV_NR, sizeof(*exec_argv));
	if (!exec_argv) {
		free(load_argv);
		return;
	}

	/*len of following strings is known at compile time */
	idx = 0;
#ifdef USE_HOST_DEBUG
	load_argv[idx] = strdup("/bin/echo");
	exec_argv[idx] = strdup(load_argv[idx]);
#else
	load_argv[idx] = strdup(KEXEC_PATH);
	exec_argv[idx] = strdup(load_argv[idx]);
#endif
	idx++;

	load_argv[idx] = strdup("-d");
	exec_argv[idx] = strdup("-e");
	idx++;

#ifdef MEM_MIN
	load_argv[idx] = asprintf("--mem-min=0x%08x", MEM_MIN);
	idx++;
#endif

#ifdef MEM_MAX
	load_argv[idx] = asprintf("--mem-max==0x%08x", MEM_MAX);
	idx++;
#endif

#ifdef USE_HARDBOOT
	load_argv[idx] = strdup("--load-hardboot");
#else
	load_argv[idx] = strdup("-l");
#endif
	idx++;

#ifdef USE_ATAGS
	load_argv[idx] = strdup("--atags");
	idx++;
#endif

#ifdef USE_NO_DTB
	load_argv[idx] = strdup("--no-dtb");
	idx++;
#endif

#ifdef USE_NO_CHECKS
	load_argv[idx] = strdup("-i");
	idx++;
#endif

#ifdef USE_KEXEC_FILE_SYSCALL
	load_argv[idx] = strdup("-s");
	idx++;
#elif defined(USE_KEXEC_SYSCALL)
	load_argv[idx] = strdup("-c");
	idx++;
#endif

	/* size is only known at runtime */
	item = params->bootcfg->list[choice];


	/* fll '--command-line' option */
	if (item->device) {
		/* default device to mount */
		strcpy(mount_dev, item->device);

		/* default fstype to mount */
		strcpy(mount_fstype, item->fstype);

		if (item->cmdline) {
			add_cmd_option(load_argv, str_cmdline_start, item->cmdline, &idx);
		} else {
			/* allocate space FIXME */
			cmdline_arg = malloc(MAX_ARG_LEN);
			if (NULL == cmdline_arg)
				perror("Can't allocate memory for cmdline_arg");

			strcpy(cmdline_arg, str_cmdline_start);	/* --command-line= */
			strcat(cmdline_arg, "root=");

			if (item->fstype) {

				/* inject extra tags for UBI */
				if (!check_for_ubi(item, cmdline_arg,
						   mount_dev, mount_fstype,
						   "ubi0", " ubi.mtd=",
						   str_mtd_id, str_ubimtd_off))
					strcat(cmdline_arg, item->device);

				strcat(cmdline_arg, " rootfstype=");
				strcat(cmdline_arg, mount_fstype);
			}

			strcat(cmdline_arg, " rootwait");

			if (params->cfg->mtdparts) {
				strcat(cmdline_arg, str_mtdparts);
				strcat(cmdline_arg, params->cfg->mtdparts);
			}

			if (params->cfg->fbcon) {
				strcat(cmdline_arg, str_fbcon);
				strcat(cmdline_arg, params->cfg->fbcon);
			}

			if (item->cmdline_append) {
				strcat(cmdline_arg, " ");
				strcat(cmdline_arg, item->cmdline_append);
			}
			load_argv[idx] = cmdline_arg;
			++idx;
		}
	}

	add_cmd_option(load_argv, "--dtb=", item->dtbpath, &idx);
	add_cmd_option(load_argv, "--initrd=", item->initrd, &idx);
	add_cmd_option(load_argv, NULL, item->kernelpath, &idx);

	for(u = 0; u < idx; u++) {
		DPRINTF("load_argv[%d]: %s", u, load_argv[u]);
	}

	/* Mount boot device */
	if ( -1 == mount(mount_dev, mount_point, mount_fstype,
			MS_RDONLY, NULL) ) {
		perror("Can't mount boot device");
		exit(-1);
	}

	/* Load kernel */
	n = fexecw(load_argv[0], (char *const *)load_argv, envp);
	if (-1 == n) {
		perror("Kexec can't load kernel");
		exit(-1);
	}

	umount(mount_point);

	dispose(cmdline_arg);

	/* Check /proc/sys/net presence */
	if ( -1 == stat("/proc/sys/net", &sinfo) ) {
		if (ENOENT == errno) {
			/* We have no network, don't issue ifdown() while kexec'ing */
			exec_argv[2] = "-x";
			DPRINTF("No network is detected, disabling ifdown()");
		} else {
			perror("Can't stat /proc/sys/net");
		}
	}

	DPRINTF("exec_argv: %s, %s, %s, %s", exec_argv[0],
			exec_argv[1], exec_argv[2], exec_argv[3]);

	/* Boot new kernel */
	execve(exec_argv[0], (char *const *)exec_argv, envp);

free:
	dispose(cmdline_arg);
	for (idx = 0; idx < MAX_LOAD_ARGV_NR; idx++)
		free(load_argv[idx]);
	dispose(load_argv);
	for (idx = 0; idx < MAX_EXEC_ARGV_NR; idx++)
		free(exec_argv[idx]);
	dispose(exec_argv);
}


int scan_devices(struct params_t *params)
{
	struct charlist *fl;
	struct bootconf_t *bootconf;
	struct device_t dev;
	struct cfgdata_t cfgdata;
	int rc,n;
	FILE *f;

	char mount_dev[16];
	char mount_fstype[16];
	char str_mtd_id[3];

#ifdef USE_ICONS
	kx_cfg_section *sc;
	int i;
	int rows;
	char **xpm_data;

#endif

	bootconf = create_bootcfg(4);
	if (NULL == bootconf) {
		DPRINTF("Can't allocate bootconf structure");
		return -1;
	}

	f = devscan_open(&fl);
	if (NULL == f) {
		log_msg(lg, "Can't initiate device scan");
		return -1;
	}

	for (;;) {
		rc = devscan_next(f, fl, &dev);
		if (rc < 0) continue;	/* Error */
		if (0 == rc) break;		/* EOF */

		/* initialize with defaults */
		strcpy(mount_dev, dev.device);
		strcpy(mount_fstype, dev.fstype);

		/* We found an ubi erase counter */
		if (!strncmp(dev.fstype, "ubi",3)) {

			/* attach ubi boot device - mtd id [0-15] */
			if(isdigit(atoi(dev.device+strlen(dev.device)-2))) {
				strcpy(str_mtd_id, dev.device+strlen(dev.device)-2);
				strcat(str_mtd_id, dev.device+strlen(dev.device)-1);
			} else {
				strcpy(str_mtd_id, dev.device+strlen(dev.device)-1);
			}
			n = ubi_attach(str_mtd_id);

			/* we have attached ubiX and we mount /dev/ubiX_0  */
			sprintf(mount_dev, "/dev/ubi%d", n);
			 /* HARDCODED: first volume */
			strcat(mount_dev, "_0");

			/* HARDCODED: we assume it's ubifs */
			strcpy(mount_fstype, "ubifs");
		}

		/* Mount device */
		if (-1 == mount(mount_dev, MOUNTPOINT, mount_fstype, MS_RDONLY, NULL)) {
			log_msg(lg, "+ can't mount device %s: %s", mount_dev, ERRMSG);
			goto free_device;
		}

		/* NOTE: Don't go out before umount'ing */

		/* Search boot method and return boot info */
		rc = get_bootinfo(&cfgdata);

		if (-1 == rc) {	/* Error */
			goto umount;
		}

#ifdef USE_ICONS
		/* Iterate over sections found */
		if (params->gui) {
			for (i = 0; i < cfgdata.count; i++) {
				sc = cfgdata.list[i];
				if (!sc) continue;

				/* Load custom icon */
				if (sc->iconpath) {
					rows = xpm_load_image(&xpm_data, sc->iconpath);
					if (-1 == rows) {
						log_msg(lg, "+ can't load xpm icon %s", sc->iconpath);
						continue;
					}

					sc->icondata = xpm_parse_image(xpm_data, rows);
					if (!sc->icondata) {
						log_msg(lg, "+ can't parse xpm icon %s", sc->iconpath);
						continue;
					}
					xpm_destroy_image(xpm_data, rows);
				}
			}
		}
#endif

umount:
		/* Umount device */
		if (-1 == umount(MOUNTPOINT)) {
			log_msg(lg, "+ can't umount device: %s", ERRMSG);
			goto free_cfgdata;
		}

		if (-1 == rc) {	/* Error */
			goto free_cfgdata;
		}

		/* Now we have something in cfgdata */
		rc = addto_bootcfg(bootconf, &dev, &cfgdata);

free_cfgdata:
		destroy_cfgdata(&cfgdata);
free_device:
		free(dev.device);
	}

	free_charlist(fl);
	params->bootcfg = bootconf;
	return 0;
}


/* Create system menu */
kx_menu *build_menu(struct params_t *params)
{
	kx_menu *menu;
	kx_menu_level *ml;
	kx_menu_item *mi;

#ifdef USE_ICONS
	kx_picture **icons;

	if (params->gui) icons = params->gui->icons;
	else icons = NULL;
#endif

	/* Create menu with 2 levels (main and system) */
	menu = menu_create(2);
	if (!menu) {
		DPRINTF("Can't create menu");
		return NULL;
	}

	/* Create main menu level */
	menu->top = menu_level_create(menu, 4, NULL);

	/* Create system menu level */
	ml = menu_level_create(menu, 6, menu->top);
	if (!ml) {
		DPRINTF("Can't create system menu");
		return menu;
	}

	mi = menu_item_add(menu->top, A_SUBMENU, "System menu", NULL, ml);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_SYSTEM]);
#endif

	mi = menu_item_add(ml, A_PARENTMENU, "Back", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_BACK]);
#endif

	mi = menu_item_add(ml, A_RESCAN, "Rescan", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_RESCAN]);
#endif

	mi = menu_item_add(ml, A_DEBUG, "Show debug info", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_DEBUG]);
#endif

	mi = menu_item_add(ml, A_REBOOT, "Reboot", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_REBOOT]);
#endif

	mi = menu_item_add(ml, A_SHUTDOWN, "Shutdown", NULL, NULL);
#ifdef USE_ICONS
	if (icons) menu_item_set_data(mi, icons[ICON_SHUTDOWN]);
#endif

	if (!initmode) {
		mi = menu_item_add(ml, A_EXIT, "Exit", NULL, NULL);
#ifdef USE_ICONS
		if (icons) menu_item_set_data(mi, icons[ICON_EXIT]);
#endif
	}

	menu->current = menu->top;
	menu_item_select(menu, 0);
	return menu;
}


/* Fill main menu with boot items */
int fill_menu(struct params_t *params)
{
	kx_menu_item *mi;
	int i, b_items, max_pri, max_i, *a;
	struct boot_item_t *tbi;
	struct bootconf_t *bl;
	const int sizeof_desc = 160;
	char *desc, *label;
#ifdef USE_ICONS
	kx_picture *icon;
	struct gui_t *gui;

	gui = params->gui;
#endif

	bl = params->bootcfg;

	if ( (NULL != bl) && (bl->fill > 0) ) b_items = bl->fill;
	else {
		log_msg(lg, "No items for menu found");
		return 0;
	}

	log_msg(lg, "Populating menu: %d item(s)", b_items);

	desc = malloc(sizeof_desc);
	if (NULL == desc) {
		DPRINTF("Can't allocate item description");
		goto dirty_exit;
	}

	a = malloc(b_items * sizeof(*a));	/* Markers array */
	if (NULL == a) {
		DPRINTF("Can't allocate markers array");
		goto dirty_exit;
	}

	for (i = 0; i < b_items; i++) a[i] = 0;	/* Clean markers array */

	/* Create menu of sorted by priority boot items */
	max_i = -1;
	for(;;) {
		max_pri = -1;
		/* Search item with maximum priority */
		for (i = 0; i < b_items; i++) {
			if (0 == a[i]) {	/* Check that item is not processed yet */
				tbi = bl->list[i];
				if (tbi->priority > max_pri) {
					max_pri = tbi->priority;	/* Max priority */
					max_i = i;					/* Max priority item index */
				}
			}
		}

		if (max_pri >= 0) {
			a[max_i] = 1;	/* Mark item as processed */
			/* We have found new max priority - insert into menu */
			tbi = bl->list[max_i];
			snprintf(desc, sizeof_desc, "%s %s %lluMb",
					tbi->device, tbi->fstype, tbi->blocks/1024);

			if (tbi->label)
				label = tbi->label;
			else
				label = tbi->kernelpath + sizeof(MOUNTPOINT) - 1;

			log_msg(lg, "+ [%s]", label);
			mi = menu_item_add(params->menu->top, A_DEVICES + max_i,
					label, desc, NULL);

#ifdef USE_ICONS
			if (gui) {
				/* Search associated with boot item icon if any */
				icon = tbi->icondata;
				if (!icon && (gui->icons)) {
					/* We have no custom icon - use default */
					switch (tbi->dtype) {
					case DVT_STORAGE:
						icon = gui->icons[ICON_STORAGE];
						break;
					case DVT_MMC:
						icon = gui->icons[ICON_MMC];
						break;
					case DVT_MTD:
						icon = gui->icons[ICON_MEMORY];
						break;
					case DVT_UNKNOWN:
					default:
						break;
					}
				}

				/* Add icon to menu */
				if (mi) mi->data = icon;
			}
#endif
		}

		if (-1 == max_pri) break;	/* We have no items to process */
	}

	free(a);
	free(desc);
	return 0;

dirty_exit:
	dispose(desc);
	return -1;
}


/* Return 0 if we are ordinary app or 1 if we are init */
int do_init(void)
{
	/* When our pid is 1 we are init-process */
	if ( 1 != getpid() ) {
		return 0;
	}

	log_msg(lg, "I'm the init-process!");

#ifdef USE_DEVTMPFS
	if (-1 == mount("devtmpfs", "/dev", "devtmpfs",
			0, NULL) ) {
		perror("Can't mount devtmpfs");
	}
#endif

	/* Mount procfs */
	if ( -1 == mount("proc", "/proc", "proc",
			0, NULL) ) {
		perror("Can't mount procfs");
		exit(-1);
	}

	/* Mount sysfs */
	if ( -1 == mount("sysfs", "/sys", "sysfs",
			0, NULL) ) {
		perror("Can't mount sysfs");
		exit(-1);
	}

	FILE *f;
	/* Set up console loglevel */
	f = fopen("/proc/sys/kernel/printk", "w");
	if (NULL == f) {
		/* CONFIG_PRINTK may be disabled */
		log_msg(lg, "/proc/sys/kernel/printk", ERRMSG);
	} else {
		fputs("0 4 1 7\n", f);
		fclose(f);
	}

	return 1;
}


int do_rescan(struct params_t *params)
{
	int i;

	/* Clean top menu level except system menu item */
	/* FIXME should be done by some function from menu module */
	kx_menu_item *mi;
	for (i = 1; i < params->menu->top->count; i++) {
		mi = params->menu->top->list[i];
		if (mi) {
			dispose(mi->label);
			dispose(mi->description);
			free(mi);
		}
		params->menu->top->list[i] = NULL;
	}
	params->menu->top->count = 1;

#ifdef USE_ICONS
	/* Destroy icons */
	/* FIXME should be done by some function from devicescan module */
	for (i = 0; i < params->bootcfg->fill; i++) {
		fb_destroy_picture(params->bootcfg->list[i]->icondata);
	}
#endif

	free_bootcfg(params->bootcfg);
	params->bootcfg = NULL;
	scan_devices(params);

	return fill_menu(params);
}


/* Process menu context
 * Return 0 to select, <0 to raise error, >0 to continue
 */
int process_ctx_menu(struct params_t *params, int action) {
	static int rc;
	static int menu_action;
	static kx_menu *menu;
	menu = params->menu;

#ifdef USE_NUMKEYS
	/* Some hacks to allow menu items selection by keys 0-9 */
	if ((action >= A_KEY0) && (action <= A_KEY9)) {
		rc = action - A_KEY0;
		if (-1 == menu_item_select_by_no(menu, rc)) {
			/* There is no item with such number - do nothing */
			return 1;
		} else {
			action = A_SELECT;
		}
	}
#endif

	menu_action = (A_SELECT == action ? menu->current->current->id : action);
	rc = 1;

	switch (menu_action) {
	case A_UP:
		menu_item_select(menu, -1);
		break;
	case A_DOWN:
		menu_item_select(menu, 1);
		break;
	case A_SUBMENU:
		menu->current = menu->current->current->submenu;
		break;
	case A_PARENTMENU:
		menu->current = menu->current->parent;
		break;

	case A_REBOOT:
#ifdef USE_FBMENU
		gui_show_msg(params->gui, "Rebooting...");
#endif
#ifdef USE_TEXTUI
		tui_show_msg(params->tui, "Rebooting...");
#endif
#ifdef USE_HOST_DEBUG
		sleep(1);
#else
		sync();
		/* if ( -1 == reboot(LINUX_REBOOT_CMD_RESTART) ) { */
		if ( -1 == reboot(RB_AUTOBOOT) ) {
			log_msg(lg, "Can't initiate reboot: %s", ERRMSG);
		}
#endif
		break;
	case A_SHUTDOWN:
#ifdef USE_FBMENU
		gui_show_msg(params->gui, "Shutting down...");
#endif
#ifdef USE_TEXTUI
		tui_show_msg(params->tui, "Shutting down...");
#endif
#ifdef USE_HOST_DEBUG
		sleep(1);
#else
		sync();
		/* if ( -1 == reboot(LINUX_REBOOT_CMD_POWER_OFF) ) { */
		if ( -1 == reboot(RB_POWER_OFF) ) {
			log_msg(lg, "Can't initiate shutdown: %s", ERRMSG);
		}
#endif
		break;

	case A_RESCAN:
#ifdef USE_FBMENU
		gui_show_msg(params->gui, "Rescanning devices.\nPlease wait...");
#endif
#ifdef USE_TEXTUI
		tui_show_msg(params->tui, "Rescanning devices.\nPlease wait...");
#endif
		if (-1 == do_rescan(params)) {
			log_msg(lg, "Rescan failed");
			return -1;
		}
		menu = params->menu;
		break;

	case A_DEBUG:
		params->context = KX_CTX_TEXTVIEW;
		break;

	case A_EXIT:
		if (initmode) break;	// don't exit if we are init
	case A_ERROR:
		rc = -1;
		break;

#ifdef USE_TIMEOUT
	case A_TIMEOUT:		// timeout was reached - boot 1st kernel if exists
		menu->current = menu->top;		/* go top-level menu */
		if (menu->current->count > 1) {
			menu_item_select(menu, 0);	/* choose first item */
			menu_item_select(menu, 1);	/* and switch to next item */
			rc = 0;
		}
		break;
#endif

	default:
		if (menu_action >= A_DEVICES) rc = 0;
		break;
	}

	return rc;
}

/* Draw menu context */
void draw_ctx_menu(struct params_t *params)
{
#ifdef USE_FBMENU
	gui_show_menu(params->gui, params->menu);
#endif
#ifdef USE_TEXTUI
	tui_show_menu(params->tui, params->menu);
#endif
}


/* Process text view context
 * Return 0 to select, <0 to raise error, >0 to continue
 */
int process_ctx_textview(struct params_t *params, int action) {
	static int rc;

	rc = 1;
	switch (action) {
	case A_UP:
		if (lg->current_line_no > 0) --lg->current_line_no;
		break;
	case A_DOWN:
		if (lg->current_line_no + 1 < lg->rows->fill) ++lg->current_line_no;
		break;
	case A_SELECT:
		/* Rewind log view to top. This should make log view usable
		 * on devices with 2 buttons only (DOWN and SELECT)
		 */
		lg->current_line_no = 0;
		params->context = KX_CTX_MENU;
		break;
	case A_EXIT:
		if (initmode) break;	// don't exit if we are init
	case A_ERROR:
		rc = -1;
		break;
	}
	return rc;
}

/* Draw text view context */
void draw_ctx_textview(struct params_t *params)
{
#ifdef USE_FBMENU
	gui_show_text(params->gui, lg);
#endif
#ifdef USE_TEXTUI
	tui_show_text(params->tui, lg);
#endif
}


/* Main event loop */
int do_main_loop(struct params_t *params, kx_inputs *inputs)
{
	int rc = 0;
	int action;

	/* Start with menu context */
	params->context = KX_CTX_MENU;
	draw_ctx_menu(params);

	/* Event loop */
	do {
		/* Read events */
		action = inputs_process(inputs);
		if (action != A_NONE) {

			/* Process events in current context */
			switch (params->context) {
			case KX_CTX_MENU:
				rc = process_ctx_menu(params, action);
				break;
			case KX_CTX_TEXTVIEW:
				rc = process_ctx_textview(params, action);
			}

			/* Draw current context */
			if (rc > 0) {
				switch (params->context) {
				case KX_CTX_MENU:
					draw_ctx_menu(params);
					break;
				case KX_CTX_TEXTVIEW:
					draw_ctx_textview(params);
					break;
				}
			}
		}
		else
			rc = 1;

	/* rc: 0 - select, <0 - raise error, >0 - continue */
	} while (rc > 0);

	/* If item is selected then return his id */
	if (0 == rc) rc = params->menu->current->current->id;

	return rc;
}


int main(int argc, char **argv)
{
	int rc = 0;
	struct cfgdata_t cfg;
	struct params_t params;
	kx_inputs inputs;

	lg = log_open(16);
	log_msg(lg, "%s starting", PACKAGE_STRING);

	initmode = do_init();

	/* Get cmdline parameters */
	params.cfg = &cfg;
	init_cfgdata(&cfg);
	cfg.angle = 0;	/* No rotation by default */
	parse_cmdline(&cfg);

	kxb_ttydev = cfg.ttydev;
	setup_terminal(kxb_ttydev, &kxb_echo_state, 1);
	/* Setup function that will restore terminal when exit() will called */
	atexit(atexit_restore_terminal);

	log_msg(lg, "FB angle is %d, tty is %s", cfg.angle, cfg.ttydev);

#ifdef USE_DELAY
	/* extra delay for initializing slow SD/CF */
	sleep(USE_DELAY);
#endif

	int no_ui = 1;	/* UI presence flag */
#ifdef USE_FBMENU
	params.gui = NULL;
	if (no_ui) {
		params.gui = gui_init(cfg.angle);
		if (NULL == params.gui) {
			log_msg(lg, "Can't initialize GUI");
		} else no_ui = 0;
	}
#endif
#ifdef USE_TEXTUI
	FILE *ttyfp;
	params.tui = NULL;
	if (no_ui) {

		if (cfg.ttydev) ttyfp = fopen(cfg.ttydev, "w");
		else ttyfp = stdout;

		params.tui = tui_init(ttyfp);
		if (NULL == params.tui) {
			log_msg(lg, "Can't initialize TUI");
			if (ttyfp != stdout) fclose(ttyfp);
		} else no_ui = 0;
	}
#endif
	if (no_ui) exit(-1); /* Exit if no one UI was initialized */

	params.menu = build_menu(&params);
	params.bootcfg = NULL;
	scan_devices(&params);

	if (-1 == fill_menu(&params)) {
		exit(-1);
	}

	/* Collect input devices */
	inputs_init(&inputs, 8);
	inputs_open(&inputs);
	inputs_preprocess(&inputs);

	/* Run main event loop
	 * Return values: <0 - error, >=0 - selected item id */
	rc = do_main_loop(&params, &inputs);

#ifdef USE_FBMENU
	if (params.gui) {
		if (rc < 0) gui_clear(params.gui);
		gui_destroy(params.gui);
	}
#endif
#ifdef USE_TEXTUI
	if (params.tui) {
		tui_destroy(params.tui);
		if (ttyfp != stdout) fclose(ttyfp);
	}
#endif
	inputs_close(&inputs);
	inputs_clean(&inputs);

	log_close(lg);
	lg = NULL;

	/* rc < 0 indicate error */
	if (rc < 0) exit(rc);

	menu_destroy(params.menu, 0);

	if (rc >= A_DEVICES) {
		pre_start_kernel(&params, rc - A_DEVICES);
		start_kernel(&params, rc - A_DEVICES);
	}

	/* When we reach this point then some error has occured */
	DPRINTF("We should not reach this point!");
	exit(-1);
}
