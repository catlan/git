#ifndef COPY_H
#define COPY_H

struct repository;

#define COPY_READ_ERROR (-2)
#define COPY_WRITE_ERROR (-3)
int copy_fd(int ifd, int ofd);
int copy_file(struct repository *repo,
	      const char *dst, const char *src, int mode);
int copy_file_with_time(struct repository *repo,
			const char *dst, const char *src, int mode);

/*
 * Create `dst` as a copy-on-write clone ("reflink") of the open regular
 * file `src_fd`: the two files share their storage until one of them is
 * modified, so cloning does not copy the file data and allocates no new
 * blocks. `dst` must not exist yet; it gets the permission bits that
 * open(2) with O_CREAT and `mode` would have given it, and the timestamps
 * of the source. Where the platform clones other metadata as well, such
 * as extended attributes, the clone keeps those of the source.
 *
 * Returns 0 on success. Otherwise returns -1 with errno set, and without
 * having created `dst`: ENOTSUP if the platform or the file system cannot
 * clone files, EXDEV if `src_fd` and `dst` are on different file systems,
 * EEXIST if `dst` already exists, EPERM if the source must not be cloned.
 *
 * Not safe to call concurrently with file creation in other threads the
 * first time, on platforms where the umask has to be read explicitly.
 */
int reflink_fd(int src_fd, const char *dst, int mode);

#endif /* COPY_H */
