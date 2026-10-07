#include "test-tool.h"
#include "copy.h"

/*
 * Create <dst> as a copy-on-write clone of <src>, used by the test suite
 * to detect whether the file system of the trash directory supports it.
 */
int cmd__reflink(int argc, const char **argv)
{
	int fd;

	if (argc != 3)
		die("usage: test-tool reflink <src> <dst>");

	fd = xopen(argv[1], O_RDONLY);
	if (reflink_fd(fd, argv[2], 0666) < 0)
		die_errno("unable to clone '%s' to '%s'", argv[1], argv[2]);
	close(fd);
	return 0;
}
