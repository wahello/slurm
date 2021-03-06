/*****************************************************************************\
 *  job_container_tmpfs.c - Define job container plugin for creating a
 *			    temporary mount namespace for the job, to provide
 *			    quota based access to node local memory.
 *****************************************************************************
 *  Copyright (C) 2019-2021 Regents of the University of California
 *  Produced at Lawrence Berkeley National Laboratory
 *  Written by Aditi Gaur <agaur@lbl.gov>
 *  All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#define _GNU_SOURCE
#define _XOPEN_SOURCE 500 /* For ftw.h */
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sched.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/mount.h>
#include <linux/limits.h>
#include <semaphore.h>

#include "src/common/slurm_xlator.h"
#include "src/common/log.h"
#include "src/common/run_command.h"

#include "read_nsconf.h"

#if defined (__APPLE__)
extern slurmd_conf_t *conf __attribute__((weak_import));
#else
slurmd_conf_t *conf = NULL;
#endif

const char plugin_name[]        = "job_container tmpfs plugin";
const char plugin_type[]        = "job_container/tmpfs";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

static slurm_ns_conf_t *ns_conf = NULL;
static int step_ns_fd = -1;

static int _create_paths(uint32_t job_id,
			 char *job_mount,
			 char *ns_holder,
			 char *src_bind,
			 char *active)
{
	ns_conf = get_slurm_ns_conf();

	if (!ns_conf) {
		error("%s: Configuration not read correctly: did namespace.conf not exist?",
			__func__);
		return SLURM_ERROR;
	}

	xassert(job_mount);

	if (snprintf(job_mount, PATH_MAX, "%s/%u", ns_conf->basepath, job_id)
	    >= PATH_MAX) {
		error("%s: Unable to build job %u mount path: %m",
			__func__, job_id);
		return SLURM_ERROR;
	}

	if (ns_holder) {
		if (snprintf(ns_holder, PATH_MAX, "%s/.ns", job_mount)
		    >= PATH_MAX) {
			error("%s: Unable to build job %u ns_holder path: %m",
			      __func__, job_id);
			return SLURM_ERROR;
		}
	}

	if (src_bind) {
		if (snprintf(src_bind, PATH_MAX, "%s/.%u", job_mount, job_id)
		    >= PATH_MAX) {
			error("%s: Unable to build job %u src_bind path: %m",
			__func__, job_id);
			return SLURM_ERROR;
		}
	}

	if (active) {
		if (snprintf(active, PATH_MAX, "%s/.active", job_mount)
		    >= PATH_MAX) {
			error("%s: Unable to build job %u active path: %m",
			__func__, job_id);
			return SLURM_ERROR;
		}
	}

	return SLURM_SUCCESS;
}

extern void container_p_reconfig(void)
{
	return;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 *	are called.  Put global initialization here.
 */
extern int init(void)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
	fatal("%s is not available on this system. (mount bind limitation)", plugin_name);
#endif

	debug("%s loaded", plugin_name);

	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is removed. Clear any allocated
 *	storage here.
 */
extern int fini(void)
{
	int rc = 0;

	debug("%s unloaded", plugin_name);

#ifdef HAVE_NATIVE_CRAY
	return SLURM_SUCCESS;
#endif

	ns_conf = get_slurm_ns_conf();
	if (!ns_conf) {
		error("%s: Configuration not loaded", __func__);
		return SLURM_ERROR;
	}
	rc = umount2(ns_conf->basepath, MNT_DETACH);
	if (rc) {
		error("%s: umount2: %s failed: %s",
		      __func__, ns_conf->basepath, strerror(errno));
		return SLURM_ERROR;
	}
	free_ns_conf();

	if (step_ns_fd != -1) {
		close(step_ns_fd);
		step_ns_fd = -1;
	}

	return SLURM_SUCCESS;
}

extern int container_p_restore(char *dir_name, bool recover)
{
#ifdef HAVE_NATIVE_CRAY
	return SLURM_SUCCESS;
#endif

	ns_conf = get_slurm_ns_conf();
	if (!ns_conf) {
		error("%s: Configuration not loaded", __func__);
		return SLURM_ERROR;
	}

	debug("namepsace.conf read successfully");

	if (ns_conf->auto_basepath) {
		int fstatus;
		char *mnt_point, *p;
		mode_t omask;

		omask = umask(S_IWGRP | S_IWOTH);

		fstatus = mkdir(ns_conf->basepath, 0755);
		if (fstatus && errno != EEXIST) {
			if (ns_conf->basepath[0] != '/') {
				debug("unable to create ns directory '%s' : does not start with '/'", ns_conf->basepath);
				umask(omask);
				return SLURM_ERROR;
			}
			mnt_point = xstrdup(ns_conf->basepath);
			p = mnt_point;
			while ((p = xstrchr(p+1, '/')) != NULL) {
				*p = '\0';
				fstatus = mkdir(mnt_point, 0755);
				if (fstatus && errno != EEXIST) {
					debug("unable to create ns required directory '%s'",
					      mnt_point);
					xfree(mnt_point);
					umask(omask);
					return SLURM_ERROR;
				}
				*p='/';
			}
			xfree(mnt_point);
			fstatus = mkdir(ns_conf->basepath, 0755);
		}

		if (fstatus && errno != EEXIST) {
			debug("unable to create ns directory '%s' : %m",
			      ns_conf->basepath);
			umask(omask);
			return SLURM_ERROR;
		}
		umask(omask);

	}

#if !defined(__APPLE__) && !defined(__FreeBSD__)
	/*
	 * MS_BIND mountflag would make mount() ignore all other mountflags
	 * except MS_REC. We need MS_PRIVATE mountflag as well to make the
	 * mount (as well as all mounts inside it) private, which needs to be
	 * done by calling mount() a second time with MS_PRIVATE and MS_REC
	 * flags.
	 */
	if (mount(ns_conf->basepath, ns_conf->basepath, "xfs", MS_BIND, NULL)) {
		error("%s: Initial base mount failed, %s",
		      __func__, strerror(errno));
		return SLURM_ERROR;
	}
	if (mount(ns_conf->basepath, ns_conf->basepath, "xfs",
		  MS_PRIVATE | MS_REC, NULL)) {
		error("%s: Initial base mount failed, %s",
		      __func__, strerror(errno));
		return SLURM_ERROR;
	}
#endif
	debug3("tmpfs: Base namespace created");

	return SLURM_SUCCESS;
}

static int _mount_private_tmp(char *path)
{
	if (!path) {
		error("%s: cannot mount /tmp", __func__);
		return -1;
	}
#if !defined(__APPLE__) && !defined(__FreeBSD__)
	if (mount(NULL, "/", NULL, MS_PRIVATE|MS_REC, NULL)) {
		error("%s: making root private: failed: %s",
		      __func__, strerror(errno));
		return -1;
	}
	if (mount(path, "/tmp", NULL, MS_BIND|MS_REC, NULL)) {
		error("%s: /tmp mount failed, %s",
		      __func__, strerror(errno));
		return -1;
	}
#endif
	return 0;
}

static int _mount_private_shm(void)
{
	int rc = 0;

	rc = umount("/dev/shm");
	if (rc && errno != EINVAL) {
		error("%s: umount /dev/shm failed: %s\n",
		      __func__, strerror(errno));
		return rc;
	}
#if !defined(__APPLE__) && !defined(__FreeBSD__)
	rc = mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);
	if (rc) {
		error("%s: mounting private /dev/shm failed: %s\n",
		      __func__, strerror(errno));
		return -1;
	}
#endif
	return rc;
}

static int _rm_data(const char *path, const struct stat *st_buf,
		    int type, struct FTW *ftwbuf)
{
	if (remove(path) < 0) {
		if (type == FTW_NS)
			error("%s: Unreachable file of FTW_NS type: %s",
			      __func__, path);
		if (type == FTW_DNR)
			error("%s: Unreadable directory: %s", __func__, path);
		error("%s: could not remove path: %s: %s",
		      __func__, path, strerror(errno));
		return errno;
	}
	return 0;
}

extern int container_p_create(uint32_t job_id)
{
	char job_mount[PATH_MAX];
	char ns_holder[PATH_MAX];
	char src_bind[PATH_MAX];
	char active[PATH_MAX];
	char *result = NULL;
	int fd;
	int rc = 0;
	sem_t *sem1 = NULL;
	sem_t *sem2 = NULL;
	pid_t cpid;

#ifdef HAVE_NATIVE_CRAY
	return 0;
#endif

	if (_create_paths(job_id, job_mount, ns_holder, src_bind, active)
	    != SLURM_SUCCESS) {
		return -1;
	}

	rc = mkdir(job_mount, 0700);
	if (rc && errno != EEXIST) {
		error("%s: mkdir %s failed: %s",
		      __func__, job_mount, strerror(errno));
		return -1;
	} else if (rc && errno == EEXIST) {
		/* stat to see if .active exists */
		struct stat st;
		rc = stat(active, &st);
		if (rc) {
			/*
			 * If .active does not exist, then the directory for
			 * the job exists but namespace is not active. This
			 * should not happen normally. Throw error and exit
			 */
			error("%s: Dir %s exists but %s was not found, exiting",
			      __func__, job_mount, active);
			goto exit2;
		}
		/*
		 * If it exists, this is coming from sbcast likely,
		 * exit as success
		 */
		rc = 0;
		goto exit2;
	}

	fd = open(ns_holder, O_CREAT|O_RDWR, S_IRWXU);
	if (fd == -1) {
		error("%s: open failed %s: %s",
		      __func__, ns_holder, strerror(errno));
		rc = -1;
		goto exit2;
	}
	close(fd);

	/* run any initialization script- if any*/
	if (ns_conf->initscript) {
		result = run_command("initscript", ns_conf->initscript, NULL,
				     10000, 0, &rc);
		if (rc) {
			error("%s: init script: %s failed",
			      __func__, ns_conf->initscript);
			goto exit2;
		} else {
			debug3("initscript stdout: %s", result);
		}
	}

	rc = mkdir(src_bind, 0700);
	if (rc) {
		error("%s: mkdir failed %s, %s",
		      __func__, src_bind, strerror(errno));
		goto exit2;
	}

	sem1 = mmap(NULL, sizeof(*sem1), PROT_READ|PROT_WRITE,
		    MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (sem1 == MAP_FAILED) {
		error("%s: mmap failed: %s", __func__, strerror(errno));
		rc = -1;
		goto exit2;
	}

	sem2 = mmap(NULL, sizeof(*sem2), PROT_READ|PROT_WRITE,
		    MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (sem2 == MAP_FAILED) {
		error("%s: mmap failed: %s", __func__, strerror(errno));
		sem_destroy(sem1);
		munmap(sem1, sizeof(*sem1));
		rc = -1;
		goto exit2;
	}

	rc = sem_init(sem1, 1, 0);
	if (rc) {
		error("%s: sem_init: %s", __func__, strerror(errno));
		goto exit1;
	}
	rc = sem_init(sem2, 1, 0);
	if (rc) {
		error("%s: sem_init: %s", __func__, strerror(errno));
		goto exit1;
	}

	cpid = fork();

	if (cpid == -1) {
		error("%s: fork Failed: %s\n", __func__, strerror(errno));
		rc = -1;
		goto exit1;
	}

	if (cpid == 0) {
		rc = unshare(CLONE_NEWNS);
		if (rc) {
			error("%s: %s", __func__, strerror(errno));
			goto child_exit;
		}
		if (sem_post(sem1) < 0) {
			error("%s: sem_post failed: %s",
			      __func__, strerror(errno));
			rc = -1;
			goto child_exit;
		}
		if (sem_wait(sem2) < 0) {
			error("%s: sem_wait failed %s",
			      __func__, strerror(errno));
			rc = -1;
			goto child_exit;
		}

		/*
		 * Now we have a persistent mount namespace.
		 * Mount private /tmp inside the namespace.
		 */
		if (_mount_private_tmp(src_bind) == -1) {
			rc = -1;
			goto child_exit;
		}
		/*
		 * This umount is to remove the basepath mount from being
		 * visible inside the namespace. So if a user looks up the
		 * mounts inside the job, they will only see their job mount
		 * but not the basepath mount.
		 */
		rc = umount2(ns_conf->basepath, MNT_DETACH);
		if (rc) {
			error("%s: umount2 failed: %s",
			      __func__, strerror(errno));
			goto child_exit;
		}
	child_exit:
		sem_destroy(sem1);
		munmap(sem1, sizeof(*sem1));
		sem_destroy(sem2);
		munmap(sem2, sizeof(*sem2));

		if (!rc) {
			rc = _mount_private_shm();
			if (rc)
				error("%s: could not mount private shm",
				      __func__);
		}
		exit(rc);
	} else {
		int wstatus;
		char proc_path[PATH_MAX];

		if (sem_wait(sem1) < 0) {
			error("%s: sem_Wait failed: %s",
			      __func__, strerror(errno));
			rc = -1;
			goto exit1;
		}

		if (snprintf(proc_path, PATH_MAX, "/proc/%u/ns/mnt", cpid)
		    >= PATH_MAX) {
			error("%s: Unable to build job %u /proc path: %m",
			      __func__, job_id);
			rc = -1;
			goto exit1;
		}

		/*
		 * Bind mount /proc/pid/ns/mnt to hold namespace active
		 * without a process attached to it
		 */
#if !defined(__APPLE__) && !defined(__FreeBSD__)
		rc = mount(proc_path, ns_holder, NULL, MS_BIND, NULL);
		if (rc) {
			error("%s: ns base mount failed: %s",
			      __func__, strerror(errno));
			if (sem_post(sem2) < 0)
				error("%s: Could not release semaphore: %s",
				      __func__, strerror(errno));
			goto exit1;
		}
#endif
		if (sem_post(sem2) < 0) {
			error("%s: sem_post failed: %s",
			      __func__, strerror(errno));
			goto exit1;
		}

		rc = waitpid(cpid, &wstatus, 0);
		if (rc == -1) {
			error("%s: waitpid failed", __func__);
			goto exit1;
		} else {
			if (rc == cpid)
				debug3("child exited: %d",
				       WEXITSTATUS(wstatus));
		}

		rc = 0;
	}

exit1:
	sem_destroy(sem1);
	munmap(sem1, sizeof(*sem1));
	sem_destroy(sem2);
	munmap(sem2, sizeof(*sem2));

exit2:
	if (rc) {
		/* cleanup the job mount */
		if (nftw(job_mount, _rm_data, 64, FTW_DEPTH|FTW_PHYS) < 0) {
			error("%s: Directory traversal failed: %s: %s",
			      __func__, job_mount, strerror(errno));
			return SLURM_ERROR;
		}

	}

	return rc;
}

/* Add a process to a job container, create the proctrack container to add */
extern int container_p_join_external(uint32_t job_id)
{
	char job_mount[PATH_MAX];
	char ns_holder[PATH_MAX];
	char active[PATH_MAX];
	int rc = 0;
	struct stat st;

	if (_create_paths(job_id, job_mount, ns_holder, NULL, active)
	    != SLURM_SUCCESS) {
		return -1;
	}

	/* assert that namespace is active */
	rc = stat(active, &st);
	if (rc) {
		/*
		 * If .active does not exist, then dont pass
		 * the fd of the namespace. It means perhaps
		 * namespace may not have been fully set up when
		 * the request for the fd came in.
		 */
		debug("%s not found, namespace cannot be joined", active);
		return -1;
	}

	if (step_ns_fd == -1) {
		step_ns_fd = open(ns_holder, O_RDONLY);
		if (step_ns_fd == -1)
			error("%s: %s", __func__, strerror(errno));
	}

	return step_ns_fd;
}

/* Add proctrack container (PAGG) to a job container */
extern int container_p_add_cont(uint32_t job_id, uint64_t cont_id)
{
	return SLURM_SUCCESS;
}

/* Call getpid() inside it */
/* Add a process to a job container, create the proctrack container to add */
extern int container_p_join(uint32_t job_id, uid_t uid)
{
	char job_mount[PATH_MAX];
	char ns_holder[PATH_MAX];
	char src_bind[PATH_MAX];
	char active[PATH_MAX];
	int fd;
	int rc = 0;

#ifdef HAVE_NATIVE_CRAY
	return SLURM_SUCCESS;
#endif

	/*
	 * Jobid 0 means we are not a real job, but a script running instead we
	 * do not need to handle this request.
	 */
	if (job_id == 0)
		return SLURM_SUCCESS;

	if (_create_paths(job_id, job_mount, ns_holder, src_bind, active)
	    != SLURM_SUCCESS) {
		return SLURM_ERROR;
	}

	rc = chown(src_bind, uid, -1);
	if (rc) {
		error("%s: chown failed for %s: %s",
		      __func__, src_bind, strerror(errno));
		return SLURM_ERROR;
	}

	/* This is called on the slurmd so we can't use ns_fd. */
	fd = open(ns_holder, O_RDONLY);
	if (fd == -1) {
		error("%s: open failed for %s: %s",
		      __func__, ns_holder, strerror(errno));
		return SLURM_ERROR;
	}

	rc = setns(fd, CLONE_NEWNS);
	if (rc) {
		error("%s: setns failed for %s: %s",
		      __func__, ns_holder, strerror(errno));
		/* closed after strerror(errno) */
		close(fd);
		return SLURM_ERROR;
	} else {
		/* touch .active to imply namespace is active */
		close(fd);
		fd = open(active, O_CREAT|O_RDWR, S_IRWXU);
		if (fd == -1) {
			error("%s: open failed %s: %s",
			      __func__, active, strerror(errno));
			return SLURM_ERROR;
		}
		close(fd);
		debug3("job entered namespace");
	}

	return SLURM_SUCCESS;
}

extern int container_p_delete(uint32_t job_id)
{
	char job_mount[PATH_MAX];
	char ns_holder[PATH_MAX];
	int rc = 0;

#ifdef HAVE_NATIVE_CRAY
	return SLURM_SUCCESS;
#endif

	if (_create_paths(job_id, job_mount, ns_holder, NULL, NULL)
	    != SLURM_SUCCESS) {
		return SLURM_ERROR;
	}

	errno = 0;
	rc = umount2(ns_holder, MNT_DETACH);
	if (rc) {
		error("%s: umount2 %s failed: %s",
		      __func__, ns_holder, strerror(errno));
		return SLURM_ERROR;
	}

	/*
	 * Traverses the job directory, and delete all files.
	 * Doesn't -
	 *	traverse filesystem boundaries,
	 *	follow symbolic links
	 * Does -
	 *	a post order traversal and delete directory after processing
	 *      contents
	 */
	if (nftw(job_mount, _rm_data, 64, FTW_DEPTH|FTW_PHYS) < 0) {
		error("%s: Directory traversal failed: %s: %s",
		      __func__, job_mount, strerror(errno));
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
