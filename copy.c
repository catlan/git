#include "git-compat-util.h"
#include "copy.h"
#include "path.h"
#include "gettext.h"
#include "strbuf.h"
#include "abspath.h"

#ifdef HAVE_CLONEFILE
#include <sys/clonefile.h>
#endif
#ifdef HAVE_FICLONE
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

int copy_fd(int ifd, int ofd)
{
	while (1) {
		char buffer[8192];
		ssize_t len = xread(ifd, buffer, sizeof(buffer));
		if (!len)
			break;
		if (len < 0)
			return COPY_READ_ERROR;
		if (write_in_full(ofd, buffer, len) < 0)
			return COPY_WRITE_ERROR;
	}
	return 0;
}

static int copy_times(const char *dst, const char *src)
{
	struct stat st;
	struct utimbuf times;
	if (stat(src, &st) < 0)
		return -1;
	times.actime = st.st_atime;
	times.modtime = st.st_mtime;
	if (utime(dst, &times) < 0)
		return -1;
	return 0;
}

int copy_file(struct repository *repo,
	      const char *dst, const char *src, int mode)
{
	int fdi, fdo, status;

	mode = (mode & 0111) ? 0777 : 0666;
	if ((fdi = open(src, O_RDONLY)) < 0)
		return fdi;
	if ((fdo = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode)) < 0) {
		close(fdi);
		return fdo;
	}
	status = copy_fd(fdi, fdo);
	switch (status) {
	case COPY_READ_ERROR:
		error_errno("copy-fd: read returned");
		break;
	case COPY_WRITE_ERROR:
		error_errno("copy-fd: write returned");
		break;
	}
	close(fdi);
	if (close(fdo) != 0)
		return error_errno("%s: close error", dst);

	if (!status && adjust_shared_perm(repo, dst))
		return -1;

	return status;
}

int copy_file_with_time(struct repository *repo,
			const char *dst, const char *src, int mode)
{
	int status = copy_file(repo, dst, src, mode);
	if (!status)
		return copy_times(dst, src);
	return status;
}

#if defined(HAVE_CLONEFILE)

#ifndef CLONE_NOOWNERCOPY
#define CLONE_NOOWNERCOPY 0
#endif

static mode_t get_umask(void)
{
	static int cached = -1;

	if (cached < 0) {
		mode_t mask = umask(0);
		umask(mask);
		cached = mask;
	}
	return cached;
}

int reflink_fd(int src_fd, const char *dst, int mode)
{
	struct stat st;
	int saved_errno;

	if (fstat(src_fd, &st) < 0)
		return -1;
	/*
	 * Cloning copies the file flags; a clone of an immutable or
	 * append-only file could not be modified later.
	 */
	if (st.st_flags & (UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND)) {
		errno = EPERM;
		return -1;
	}

	/*
	 * fclonefileat(2) refuses to overwrite an existing `dst`, copies
	 * the timestamps of the source and, with CLONE_NOOWNERCOPY, makes
	 * the caller the owner of the clone. It also copies the permission
	 * bits and the file flags of the source, where a regular open(2)
	 * would have applied the umask to `mode` and set no flags; do that
	 * by hand.
	 */
	if (fclonefileat(src_fd, AT_FDCWD, dst,
			 CLONE_NOFOLLOW | CLONE_NOOWNERCOPY) < 0)
		return -1;
	mode &= ~get_umask();
	if (((st.st_mode & 07777) != mode && chmod(dst, mode) < 0) ||
	    (st.st_flags && chflags(dst, 0) < 0)) {
		saved_errno = errno;
		unlink(dst);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

#elif defined(HAVE_FICLONE)

int reflink_fd(int src_fd, const char *dst, int mode)
{
	struct stat st;
	struct timespec times[2];
	int dst_fd, saved_errno;

	if (fstat(src_fd, &st) < 0)
		return -1;
	if (!S_ISREG(st.st_mode)) {
		errno = EINVAL;
		return -1;
	}

	dst_fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode);
	if (dst_fd < 0)
		return -1;

	times[0] = st.st_atim;
	times[1] = st.st_mtim;
	if (ioctl(dst_fd, FICLONE, src_fd) < 0 ||
	    futimens(dst_fd, times) < 0) {
		saved_errno = errno;
		close(dst_fd);
		unlink(dst);
		errno = saved_errno;
		return -1;
	}
	if (close(dst_fd) < 0) {
		saved_errno = errno;
		unlink(dst);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

#else

int reflink_fd(int src_fd UNUSED, const char *dst UNUSED, int mode UNUSED)
{
	errno = ENOTSUP;
	return -1;
}

#endif
