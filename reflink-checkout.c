#include "git-compat-util.h"
#include "reflink-checkout.h"
#include "convert.h"
#include "copy.h"
#include "entry.h"
#include "hash.h"
#include "object-file.h"
#include "read-cache-ll.h"
#include "repository.h"
#include "strbuf.h"
#include "thread-utils.h"
#include "trace2.h"
#include "wrapper.h"

/*
 * Cloning a file is a fixed-cost operation of the file system that is
 * comparable to writing a small file, but it parallelizes well, as does
 * verifying the contents of the clones, so spread the work over threads
 * once there is enough of it.
 */
#define MAX_PARALLEL 8
#define THREAD_COST 500

struct reflink_item {
	struct cache_entry *ce;
	struct stat st;
	unsigned cloned:1;
};

struct reflink_data {
	pthread_t pthread;
	const struct git_hash_algo *algop;
	const char *source;
	struct reflink_item *items;
	size_t nr;
	/* set once cloning turned out not to work here; private to the thread */
	int unsupported;
};

/*
 * Hash the contents of `fd` as a blob, without any conversion.
 */
static int hash_blob_fd(const struct git_hash_algo *algop, int fd,
			size_t size, struct object_id *oid)
{
	struct git_hash_ctx ctx;
	char hdr[MAX_HEADER_LEN];
	char buf[65536];
	int hdrlen;

	hdrlen = format_object_header(hdr, sizeof(hdr), OBJ_BLOB, size);
	git_hash_init(&ctx, algop);
	git_hash_update(&ctx, hdr, hdrlen);
	while (size) {
		ssize_t len = xread(fd, buf, size < sizeof(buf) ? size : sizeof(buf));
		if (len <= 0) {
			git_hash_discard(&ctx);
			return -1;
		}
		git_hash_update(&ctx, buf, len);
		size -= len;
	}
	git_hash_final_oid(oid, &ctx);
	return 0;
}

/*
 * Returns 0 if `item` was created as a verified clone, -1 otherwise.
 */
static int reflink_item(struct reflink_data *d, struct reflink_item *item)
{
	const struct cache_entry *ce = item->ce;
	struct strbuf path = STRBUF_INIT;
	struct object_id oid;
	struct stat st;
	size_t size;
	int fd;

	/*
	 * Only regular files are of interest. Check before opening, as
	 * opening e.g. a FIFO would block, and do not block if the file
	 * changes in between either.
	 */
	strbuf_addf(&path, "%s/%s", d->source, ce->name);
	if (lstat(path.buf, &st) || !S_ISREG(st.st_mode)) {
		strbuf_release(&path);
		return -1;
	}
	fd = open_nofollow(path.buf, O_RDONLY | O_NONBLOCK);
	strbuf_release(&path);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}

	if (reflink_fd(fd, ce->name, (ce->ce_mode & 0100) ? 0777 : 0666) < 0) {
		/*
		 * Unless the problem is with this particular file, the file
		 * system or the platform cannot clone; stop trying.
		 */
		if (errno != EEXIST && errno != EPERM &&
		    errno != EACCES && errno != EINVAL)
			d->unsupported = 1;
		close(fd);
		return -1;
	}
	close(fd);

	/*
	 * A modification time in the future would make the entry racy
	 * forever; such a clone gets the current time, like any other
	 * file we write.
	 */
	if (st.st_mtime > time(NULL))
		utime(ce->name, NULL);

	/*
	 * The original may be modified, or may never have had the
	 * contents of the blob in the first place. The clone is a
	 * consistent snapshot of it, so check that one, and record the
	 * stat data of exactly what was checked.
	 */
	fd = open_nofollow(ce->name, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		goto discard;
	if (fstat(fd, &item->st) || !S_ISREG(item->st.st_mode)) {
		close(fd);
		goto discard;
	}
	size = item->st.st_size;
	if ((off_t)size != item->st.st_size ||
	    hash_blob_fd(d->algop, fd, size, &oid)) {
		close(fd);
		goto discard;
	}
	close(fd);
	if (!oideq(&oid, &ce->oid))
		goto discard;
	return 0;

discard:
	unlink(ce->name);
	return -1;
}

static void *reflink_thread(void *data)
{
	struct reflink_data *d = data;
	size_t i;

	for (i = 0; i < d->nr && !d->unsupported; i++)
		if (!reflink_item(d, &d->items[i]))
			d->items[i].cloned = 1;
	return NULL;
}

void reflink_checkout_entries(const struct checkout *state,
			      struct index_state *index,
			      const char *source)
{
	struct reflink_item *items;
	struct reflink_data data[MAX_PARALLEL];
	struct reflink_data probe = {
		.algop = index->repo->hash_algo,
		.source = source,
	};
	struct strbuf path = STRBUF_INIT;
	size_t nr = 0, i, offset, work, threads;
	intmax_t nr_cloned = 0;

	if (!state->refresh_cache || state->base_dir_len)
		return;

	trace2_region_enter("checkout", "reflink", index->repo);

	/*
	 * Attributes and the lstat() cache behind create_directories()
	 * are not safe to use from threads; sort those parts out first.
	 */
	ALLOC_ARRAY(items, index->cache_nr);
	for (i = 0; i < index->cache_nr && !probe.unsupported; i++) {
		struct cache_entry *ce = index->cache[i];
		struct conv_attrs ca;
		struct stat st;

		if (!(ce->ce_flags & CE_UPDATE) || !S_ISREG(ce->ce_mode))
			continue;
		convert_attrs(index, &ca, ce->name);
		if (!conv_attrs_checkout_is_passthrough(&ca))
			continue;
		if (!lstat(ce->name, &st) || errno != ENOENT)
			continue;

		strbuf_reset(&path);
		strbuf_add(&path, ce->name, ce_namelen(ce));
		create_directories(path.buf, path.len, state);

		items[nr].ce = ce;
		items[nr].cloned = 0;
		nr++;

		/*
		 * Find out with the first candidate whether cloning works
		 * here at all, before spending more effort on the rest. This
		 * also reads the umask, which must not happen in a thread.
		 */
		if (nr == 1) {
			probe.items = items;
			probe.nr = 1;
			reflink_thread(&probe);
		}
	}
	strbuf_release(&path);

	/* the probe took care of the first item */
	threads = nr > 1 ? (nr - 1) / THREAD_COST : 0;
	if (threads > MAX_PARALLEL)
		threads = MAX_PARALLEL;
	if (threads > (size_t)online_cpus())
		threads = online_cpus();
	if (!HAVE_THREADS || threads < 2)
		threads = 1;

	memset(data, 0, sizeof(data));
	work = nr > 1 ? DIV_ROUND_UP(nr - 1, threads) : 0;
	for (i = 0, offset = 1; i < threads; i++, offset += work) {
		struct reflink_data *d = &data[i];

		d->algop = probe.algop;
		d->source = source;
		if (offset >= nr)
			continue;
		d->items = items + offset;
		d->nr = offset + work <= nr ? work : nr - offset;
	}
	if (nr <= 1 || probe.unsupported) {
		/* nothing left to do */
	} else if (threads == 1) {
		reflink_thread(&data[0]);
	} else {
		for (i = 0; i < threads; i++) {
			int err = pthread_create(&data[i].pthread, NULL,
						 reflink_thread, &data[i]);
			if (err)
				die(_("unable to create reflink thread: %s"),
				    strerror(err));
		}
		for (i = 0; i < threads; i++)
			if (pthread_join(data[i].pthread, NULL))
				die(_("unable to join reflink thread"));
	}

	for (i = 0; i < nr; i++) {
		if (!items[i].cloned)
			continue;
		update_ce_after_write(state, items[i].ce, &items[i].st);
		nr_cloned++;
	}
	free(items);
	trace2_region_leave("checkout", "reflink", index->repo);

	trace2_data_intmax("checkout", index->repo, "reflink/candidates", nr);
	trace2_data_intmax("checkout", index->repo, "reflink/cloned", nr_cloned);
}
