#ifndef REFLINK_CHECKOUT_H
#define REFLINK_CHECKOUT_H

struct checkout;
struct index_state;

/*
 * Populate the paths of `index` that are marked for checkout (CE_UPDATE)
 * and do not exist in the working tree yet with copy-on-write clones of
 * the files at the same paths below `source`, another checkout of the
 * repository, where the file system supports that. Only clones whose
 * contents are exactly those of the blob to be checked out are kept and
 * recorded in the index as up to date; anything else is left for the
 * caller to write as usual. Silently does nothing where cloning is not
 * possible.
 */
void reflink_checkout_entries(const struct checkout *state,
			      struct index_state *index,
			      const char *source);

#endif /* REFLINK_CHECKOUT_H */
