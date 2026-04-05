#!/bin/sh

test_description='zstd compression for object storage'

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh

setup_zstd_repo () {
	git init "$1" &&
	git -C "$1" config core.repositoryFormatVersion 1 &&
	git -C "$1" config extensions.compressionFormat zstd &&
	git -C "$1" config core.compressionAlgorithm zstd
}

test_expect_success ZSTD 'loose object round-trip with zstd' '
	setup_zstd_repo zstd-loose &&
	echo "hello zstd" >zstd-loose/file.txt &&
	git -C zstd-loose add file.txt &&
	git -C zstd-loose commit -m "initial" &&
	echo "hello zstd" >expect &&
	git -C zstd-loose show HEAD:file.txt >actual &&
	test_cmp expect actual
'

test_expect_success ZSTD 'zstd magic bytes in loose object' '
	blob_oid=$(git -C zstd-loose rev-parse HEAD:file.txt) &&
	prefix=$(echo "$blob_oid" | cut -c1-2) &&
	suffix=$(echo "$blob_oid" | cut -c3-) &&
	objpath="zstd-loose/.git/objects/$prefix/$suffix" &&
	test_path_is_file "$objpath" &&
	# zstd magic: 0x28 0xb5 0x2f 0xfd
	printf "\\050\\265\\057\\375" >expect_magic &&
	test_copy_bytes 4 <"$objpath" >actual_magic &&
	test_cmp expect_magic actual_magic
'

test_expect_success ZSTD 'zlib objects readable in zstd-mode repo' '
	git init zstd-mixed &&
	echo "zlib content" >zstd-mixed/old.txt &&
	git -C zstd-mixed add old.txt &&
	git -C zstd-mixed commit -m "zlib object" &&
	git -C zstd-mixed config core.repositoryFormatVersion 1 &&
	git -C zstd-mixed config extensions.compressionFormat zstd &&
	git -C zstd-mixed config core.compressionAlgorithm zstd &&
	echo "zlib content" >expect &&
	git -C zstd-mixed show HEAD:old.txt >actual &&
	test_cmp expect actual
'

test_expect_success ZSTD 'fsck passes on zstd-compressed repo' '
	setup_zstd_repo zstd-fsck &&
	for i in $(test_seq 1 10)
	do
		echo "content $i" >"zstd-fsck/file_$i.txt" || return 1
	done &&
	git -C zstd-fsck add . &&
	git -C zstd-fsck commit -m "ten files" &&
	git -C zstd-fsck fsck
'

test_expect_success ZSTD 'repack round-trip with zstd' '
	setup_zstd_repo zstd-repack &&
	for i in $(test_seq 1 20)
	do
		echo "content $i" >"zstd-repack/file_$i.txt" || return 1
	done &&
	git -C zstd-repack add . &&
	git -C zstd-repack commit -m "twenty files" &&
	git -C zstd-repack repack -adF &&
	git -C zstd-repack fsck &&
	git -C zstd-repack log --oneline >actual &&
	test_line_count = 1 actual
'

test_expect_success !ZSTD 'error when zstd not compiled' '
	git init no-zstd &&
	test_must_fail git -C no-zstd \
		-c core.compressionAlgorithm=zstd \
		hash-object --stdin </dev/null 2>err &&
	test_grep "zstd" err
'

test_done
