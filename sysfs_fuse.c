/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 26
#endif

#define _FILE_OFFSET_BITS 64

#define __STDC_FORMAT_MACROS
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wait.h>
#include <linux/magic.h>
#include <linux/sched.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>

#include "bindings.h"
#include "memory_utils.h"
#include "cgroups/cgroup.h"
#include "config.h"
#include "sysfs_fuse.h"
#include "utils.h"

static int sys_devices_system_cpu_online_read(char *buf, size_t size,
					      off_t offset,
					      struct fuse_file_info *fi)
{
	__do_free char *cg = NULL, *cpuset = NULL;
	struct fuse_context *fc = fuse_get_context();
	struct file_info *d = INTTYPE_TO_PTR(fi->fh);
	char *cache = d->buf;
	bool use_view;

	int max_cpus = 0;
	pid_t initpid;
	ssize_t total_len = 0;

	if (offset) {
		int left;

		if (!d->cached)
			return 0;

		if (offset > d->size)
			return -EINVAL;

		left = d->size - offset;
		total_len = left > size ? size : left;
		memcpy(buf, cache + offset, total_len);

		return total_len;
	}

	initpid = lookup_initpid_in_store(fc->pid);
	if (initpid <= 0)
		initpid = fc->pid;
	cg = get_pid_cgroup(initpid, "cpuset");
	if (!cg)
		return read_file_fuse("/sys/devices/system/cpu/online", buf, size, d);
	prune_init_slice(cg);

	cpuset = get_cpuset(cg);
	if (!cpuset)
		return 0;

	use_view = cgroup_ops->can_use_cpuview(cgroup_ops);
	if (use_view)
		max_cpus = max_cpu_count(cg);

	if (max_cpus == 0)
		return read_file_fuse("/sys/devices/system/cpu/online", buf, size, d);
	if (max_cpus > 1)
		total_len = snprintf(d->buf, d->buflen, "0-%d\n", max_cpus - 1);
	else
		total_len = snprintf(d->buf, d->buflen, "0\n");
	if (total_len < 0 || total_len >= d->buflen) {
		lxcfs_error("%s\n", "failed to write to cache");
		return 0;
	}

	d->size = (int)total_len;
	d->cached = 1;

	if (total_len > size)
		total_len = size;

	memcpy(buf, d->buf, total_len);

	return total_len;
}

static off_t get_sysfile_size(const char *which)
{
	__do_fclose FILE *f = NULL;
	__do_free char *line = NULL;
	size_t len = 0;
	ssize_t sz, answer = 0;

	f = fopen(which, "re");
	if (!f)
		return 0;

	while ((sz = getline(&line, &len, f)) != -1)
		answer += sz;

	return answer;
}

int sys_getattr(const char *path, struct stat *sb)
{
	struct timespec now;

	memset(sb, 0, sizeof(struct stat));
	if (clock_gettime(CLOCK_REALTIME, &now) < 0)
		return -EINVAL;
	sb->st_uid = sb->st_gid = 0;
	sb->st_atim = sb->st_mtim = sb->st_ctim = now;
	if (strcmp(path, "/sys") == 0) {
		sb->st_mode = S_IFDIR | 00555;
		sb->st_nlink = 2;
		return 0;
	}
	if (strcmp(path, "/sys/devices") == 0) {
		sb->st_mode = S_IFDIR | 00555;
		sb->st_nlink = 2;
		return 0;
	}
	if (strcmp(path, "/sys/devices/system") == 0) {
		sb->st_mode = S_IFDIR | 00555;
		sb->st_nlink = 2;
		return 0;
	}
	if (strcmp(path, "/sys/devices/system/cpu") == 0) {
		sb->st_mode = S_IFDIR | 00555;
		sb->st_nlink = 2;
		return 0;
	}
	if (strcmp(path, "/sys/devices/system/cpu/online") == 0) {
		sb->st_size = 0;
		sb->st_mode = S_IFREG | 00444;
		sb->st_nlink = 1;
		return 0;
	}

	return -ENOENT;
}

int sys_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi)
{
	if (strcmp(path, "/sys") == 0) {
		if (filler(buf, ".", NULL, 0) != 0 ||
		    filler(buf, "..", NULL, 0) != 0 ||
		    filler(buf, "devices", NULL, 0) != 0)
			return -ENOENT;
		return 0;
	}
	if (strcmp(path, "/sys/devices") == 0) {
		if (filler(buf, ".", NULL, 0) != 0 ||
		    filler(buf, "..", NULL, 0) != 0 ||
		    filler(buf, "system", NULL, 0) != 0)
			return -ENOENT;
		return 0;
	}
	if (strcmp(path, "/sys/devices/system") == 0) {
		if (filler(buf, ".", NULL, 0) != 0 ||
		    filler(buf, "..", NULL, 0) != 0 ||
		    filler(buf, "cpu", NULL, 0) != 0)
			return -ENOENT;
		return 0;
	}
	if (strcmp(path, "/sys/devices/system/cpu") == 0) {
		if (filler(buf, ".", NULL, 0) != 0 ||
		    filler(buf, "..", NULL, 0) != 0 ||
		    filler(buf, "online", NULL, 0) != 0)
			return -ENOENT;
		return 0;
	}

	return 0;
}

int sys_open(const char *path, struct fuse_file_info *fi)
{
	__do_free struct file_info *info = NULL;
	int type = -1;

	if (strcmp(path, "/sys/devices") == 0)
		type = LXC_TYPE_SYS_DEVICES;
	if (strcmp(path, "/sys/devices/system") == 0)
		type = LXC_TYPE_SYS_DEVICES_SYSTEM;
	if (strcmp(path, "/sys/devices/system/cpu") == 0)
		type = LXC_TYPE_SYS_DEVICES_SYSTEM_CPU;
	if (strcmp(path, "/sys/devices/system/cpu/online") == 0)
		type = LXC_TYPE_SYS_DEVICES_SYSTEM_CPU_ONLINE;
	if (type == -1)
		return -ENOENT;

	info = malloc(sizeof(*info));
	if (!info)
		return -ENOMEM;

	memset(info, 0, sizeof(*info));
	info->type = type;

	info->buflen = get_sysfile_size(path) + BUF_RESERVE_SIZE;

	info->buf = malloc(info->buflen);
	if (!info->buf)
		return -ENOMEM;

	memset(info->buf, 0, info->buflen);
	/* set actual size to buffer size */
	info->size = info->buflen;

	fi->fh = PTR_TO_UINT64(move_ptr(info));
	return 0;
}

int sys_access(const char *path, int mask)
{
	if (strcmp(path, "/sys") == 0 && access(path, R_OK) == 0)
		return 0;
	if (strcmp(path, "/sys/devices") == 0 && access(path, R_OK) == 0)
		return 0;
	if (strcmp(path, "/sys/devices/system") == 0 && access(path, R_OK) == 0)
		return 0;
	if (strcmp(path, "/sys/devices/system/cpu") == 0 &&
	    access(path, R_OK) == 0)
		return 0;
	/* these are all read-only */
	if ((mask & ~R_OK) != 0)
		return -EACCES;
	return 0;
}

int sys_release(const char *path, struct fuse_file_info *fi)
{
	do_release_file_info(fi);
	return 0;
}

int sys_releasedir(const char *path, struct fuse_file_info *fi)
{
	do_release_file_info(fi);
	return 0;
}

int sys_read(const char *path, char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi)
{
	struct file_info *f = INTTYPE_TO_PTR(fi->fh);

	switch (f->type) {
	case LXC_TYPE_SYS_DEVICES_SYSTEM_CPU_ONLINE:
		return sys_devices_system_cpu_online_read(buf, size, offset, fi);
	case LXC_TYPE_SYS_DEVICES:
		break;
	case LXC_TYPE_SYS_DEVICES_SYSTEM:
		break;
	case LXC_TYPE_SYS_DEVICES_SYSTEM_CPU:
		break;
	}

	return -EINVAL;
}
