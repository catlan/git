#!/bin/sh

test_description='git worktree add populates new worktrees with file clones'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_lazy_prereq REFLINK '
	echo content >src &&
	test-tool reflink src dst
'

# Run "git worktree add" and record its trace2 events in trace.txt.
worktree_add () {
	rm -f trace.txt &&
	GIT_TRACE2_EVENT="$(pwd)/trace.txt" GIT_TRACE2_EVENT_NESTING=10 \
		git worktree add "$@"
}

# The number of files the last worktree_add cloned.
reflink_cloned () {
	sed -n 's/.*"category":"checkout","key":"reflink\/cloned","value":"\([0-9]*\)".*/\1/p' trace.txt
}

# Switch the current worktree back to main, making sure that files
# checked out under other attributes are written again.
back_to_main () {
	git checkout main &&
	rm dir/sub/text &&
	git checkout -- dir/sub/text
}

# Check that the new worktree looks exactly like a fresh checkout.
check_worktree () {
	git -C "$1" status --porcelain >status.out &&
	test_must_be_empty status.out &&
	git -C "$1" diff --quiet HEAD &&
	git ls-tree -r --name-only "${2:-HEAD}" >expect.files &&
	git -C "$1" ls-tree -r --name-only HEAD >actual.files &&
	test_cmp expect.files actual.files
}

test_expect_success 'setup' '
	test_commit initial &&
	mkdir -p dir/sub sub2 &&
	test_write_lines one two three >dir/sub/text &&
	printf "\0\1\2 binary" >dir/binary &&
	test_write_lines a b >exec &&
	test_write_lines deep >sub2/file &&
	git add dir exec sub2 &&
	test_chmod +x exec &&
	git commit -m files &&
	git ls-files >tracked &&

	test_write_lines changed >dir/sub/text &&
	echo new >added &&
	git add dir/sub/text added &&
	git commit -m "other content" &&
	git branch other &&
	git reset --hard HEAD^ &&

	git ls-files -z | xargs -0 git check-attr --all >attrs &&
	test_must_be_empty attrs
'

test_expect_success 'unmodified files are reused for the new worktree' '
	worktree_add wt &&
	check_worktree wt &&
	test_cmp dir/sub/text wt/dir/sub/text &&
	test_cmp dir/binary wt/dir/binary &&
	test -x wt/exec
'

test_expect_success REFLINK 'all eligible files were cloned' '
	test_line_count = "$(reflink_cloned)" tracked
'

test_expect_success 'cloned files are independent of the originals' '
	cp dir/sub/text expect &&
	echo more >>wt/dir/sub/text &&
	test_cmp expect dir/sub/text &&
	git -C wt checkout -- dir/sub/text &&
	test_cmp expect wt/dir/sub/text &&
	echo more >>dir/sub/text &&
	test_cmp expect wt/dir/sub/text &&
	git checkout -- dir/sub/text
'

test_expect_success REFLINK 'cloned files keep the timestamp of the original' '
	test-tool chmtime =-86400 dir/sub/text &&
	test-tool chmtime --get dir/sub/text >expect &&
	worktree_add wt-mtime &&
	test-tool chmtime --get wt-mtime/dir/sub/text >actual &&
	test_cmp expect actual
'

test_expect_success 'cloned files get the same permissions as written ones' '
	chmod 600 dir/binary &&
	test_when_finished "chmod 644 dir/binary" &&
	worktree_add wt-mode &&
	check_worktree wt-mode &&
	test_modebits wt-mode/dir/binary >actual &&
	test_modebits wt-mode/initial.t >expect &&
	test_cmp expect actual &&
	test -x wt-mode/exec
'

test_expect_success 'modified files in the current worktree are not reused' '
	test_write_lines dirty >dir/sub/text &&
	test_write_lines also dirty >exec &&
	git add exec &&
	test_when_finished "git reset --hard" &&
	worktree_add wt-dirty &&
	check_worktree wt-dirty &&
	git show HEAD:dir/sub/text >expect &&
	test_cmp expect wt-dirty/dir/sub/text &&
	git show HEAD:exec >expect &&
	test_cmp expect wt-dirty/exec
'

test_expect_success REFLINK 'only unmodified files were cloned' '
	test_line_count = "$(($(reflink_cloned) + 2))" tracked
'

test_expect_success PIPE 'a tracked path replaced by a FIFO is skipped' '
	rm dir/sub/text &&
	mkfifo dir/sub/text &&
	test_when_finished "rm -f dir/sub/text && git checkout -- dir/sub/text" &&
	worktree_add wt-fifo &&
	check_worktree wt-fifo &&
	git show HEAD:dir/sub/text >expect &&
	test_cmp expect wt-fifo/dir/sub/text
'

test_expect_success REFLINK 'clones of files dated in the future get the current time' '
	test-tool chmtime =+86400 dir/sub/text &&
	test_when_finished "test-tool chmtime =-1 dir/sub/text" &&
	worktree_add wt-future &&
	check_worktree wt-future &&
	test-tool chmtime --get dir/sub/text >future &&
	test-tool chmtime --get wt-future/dir/sub/text >actual &&
	test "$(cat actual)" -lt "$(cat future)"
'

test_expect_success 'a same-sized change with the old timestamp is noticed' '
	test_write_lines ONE two three >dir/sub/text &&
	test-tool chmtime =-86400 dir/sub/text &&
	test_when_finished "git checkout -- dir/sub/text" &&
	worktree_add wt-same-size &&
	check_worktree wt-same-size &&
	git show HEAD:dir/sub/text >expect &&
	test_cmp expect wt-same-size/dir/sub/text
'

test_expect_success 'untracked and ignored files are not carried over' '
	echo untracked >untracked &&
	echo ignored >ignored &&
	echo ignored >.git/info/exclude &&
	test_when_finished "rm -f untracked ignored" &&
	worktree_add wt-untracked &&
	check_worktree wt-untracked &&
	test_path_is_missing wt-untracked/untracked &&
	test_path_is_missing wt-untracked/ignored
'

test_expect_success 'files that differ in the target commit are written afresh' '
	worktree_add --detach wt-other other &&
	check_worktree wt-other other &&
	git show other:dir/sub/text >expect &&
	test_cmp expect wt-other/dir/sub/text &&
	git show other:added >expect &&
	test_cmp expect wt-other/added &&
	test_cmp dir/binary wt-other/dir/binary
'

test_expect_success REFLINK 'only files identical in the target commit were cloned' '
	test_line_count = "$(($(reflink_cloned) + 1))" tracked
'

test_expect_success 'setup branches with conversion attributes' '
	git checkout -b filtered main &&
	echo "dir/sub/text filter=upper" >.gitattributes &&
	git add .gitattributes &&
	git commit -m "filter attribute" &&

	git checkout -b crlf main &&
	echo "dir/sub/text text eol=crlf" >.gitattributes &&
	git add .gitattributes &&
	git commit -m "crlf attribute" &&

	git checkout -b lf main &&
	echo "* text=auto eol=lf" >.gitattributes &&
	git add .gitattributes &&
	git commit -m "lf attribute" &&

	git checkout main
'

test_expect_success 'smudge filter of the target commit is applied' '
	test_config filter.upper.smudge "tr a-z A-Z" &&
	test_config filter.upper.clean "tr A-Z a-z" &&
	worktree_add --detach wt-filtered filtered &&
	check_worktree wt-filtered filtered &&
	tr a-z A-Z <dir/sub/text >expect &&
	test_cmp expect wt-filtered/dir/sub/text
'

test_expect_success 'smudged files of the current worktree are not reused' '
	test_config filter.upper.smudge "tr a-z A-Z" &&
	test_config filter.upper.clean "tr A-Z a-z" &&
	git checkout filtered &&
	test_when_finished back_to_main &&
	rm dir/sub/text &&
	git checkout -- dir/sub/text &&
	git show main:dir/sub/text | tr a-z A-Z >smudged &&
	test_cmp smudged dir/sub/text &&
	worktree_add --detach wt-from-filtered main &&
	check_worktree wt-from-filtered main &&
	git show main:dir/sub/text >expect &&
	test_cmp expect wt-from-filtered/dir/sub/text
'

test_expect_success 'line ending conversion of the target commit is applied' '
	worktree_add --detach wt-crlf crlf &&
	check_worktree wt-crlf crlf &&
	append_cr <dir/sub/text >expect &&
	test_cmp expect wt-crlf/dir/sub/text
'

test_expect_success 'files whose attributes changed after checkout are not reused' '
	git checkout -b stale crlf &&
	test_when_finished "back_to_main && git branch -D stale" &&
	rm dir/sub/text &&
	git checkout -- dir/sub/text &&
	git show crlf:dir/sub/text | append_cr >crlf &&
	test_cmp crlf dir/sub/text &&
	git rm -q .gitattributes &&
	git commit -m "attributes removed, file untouched" &&
	test_cmp crlf dir/sub/text &&
	worktree_add wt-stale &&
	check_worktree wt-stale stale &&
	git show stale:dir/sub/text >expect &&
	test_cmp expect wt-stale/dir/sub/text
'

test_expect_success 'files with normalized line endings are reused' '
	worktree_add --detach wt-lf lf &&
	check_worktree wt-lf lf &&
	test_cmp dir/sub/text wt-lf/dir/sub/text &&
	test_cmp dir/binary wt-lf/dir/binary
'

test_expect_success REFLINK 'files with normalized line endings were cloned' '
	test_line_count = "$(reflink_cloned)" tracked
'

test_expect_success 'normalized files with CRLF in the working tree are not reused' '
	git checkout lf &&
	test_when_finished "git checkout -- dir/sub/text && back_to_main" &&
	append_cr <dir/sub/text >dir/sub/text+ &&
	mv dir/sub/text+ dir/sub/text &&
	git add dir/sub/text &&
	git status --porcelain -uno >status.out &&
	test_must_be_empty status.out &&
	worktree_add --detach wt-crlf-in-tree lf &&
	check_worktree wt-crlf-in-tree lf &&
	git show lf:dir/sub/text >expect &&
	test_cmp expect wt-crlf-in-tree/dir/sub/text
'

test_expect_success SYMLINKS 'symbolic links are recreated' '
	ln -s dir/sub/text link &&
	git add link &&
	git commit -m link &&
	test_when_finished "git reset --hard HEAD^" &&
	worktree_add wt-link &&
	check_worktree wt-link &&
	test -h wt-link/link &&
	test "$(test_readlink wt-link/link)" = dir/sub/text
'

test_expect_success 'the new worktree works when sparse-checkout is enabled' '
	git sparse-checkout set --cone dir &&
	test_when_finished "git sparse-checkout disable" &&
	test_path_is_missing sub2/file &&
	worktree_add wt-sparse &&
	git -C wt-sparse status --porcelain >status.out &&
	test_must_be_empty status.out &&
	test_path_is_file wt-sparse/dir/sub/text &&
	test_path_is_missing wt-sparse/sub2/file &&
	git -C wt-sparse ls-files -t sub2/file >actual &&
	echo "S sub2/file" >expect &&
	test_cmp expect actual
'

test_expect_success 'the new worktree works with a sparse index' '
	git sparse-checkout set --cone dir &&
	test_when_finished "git sparse-checkout disable" &&
	test_config index.sparse true &&
	git sparse-checkout reapply &&
	worktree_add wt-sparse-index &&
	git -C wt-sparse-index status --porcelain >status.out &&
	test_must_be_empty status.out &&
	test_path_is_file wt-sparse-index/dir/sub/text &&
	test_path_is_missing wt-sparse-index/sub2/file &&
	git -C wt-sparse-index ls-files -t sub2/file >actual &&
	echo "S sub2/file" >expect &&
	test_cmp expect actual
'

test_expect_success 'worktree add from a linked worktree and a subdirectory' '
	git -C wt worktree add ../wt-from-linked &&
	check_worktree wt-from-linked &&
	git -C dir/sub worktree add ../../wt-from-subdir &&
	check_worktree wt-from-subdir
'

test_expect_success '--no-checkout and --orphan leave the new worktree empty' '
	worktree_add --no-checkout wt-no-checkout &&
	test_path_is_missing wt-no-checkout/initial.t &&
	worktree_add --orphan -b orphan wt-orphan &&
	test_path_is_missing wt-orphan/initial.t
'

test_expect_success 'worktree.reflink=false disables cloning' '
	rm -f trace.txt &&
	GIT_TRACE2_EVENT="$(pwd)/trace.txt" GIT_TRACE2_EVENT_NESTING=10 \
		git -c worktree.reflink=false worktree add wt-no-reflink &&
	check_worktree wt-no-reflink &&
	test_grep ! "reflink/cloned" trace.txt
'

test_done
