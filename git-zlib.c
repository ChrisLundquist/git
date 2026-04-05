/*
 * zlib wrappers to make sure we don't silently miss errors
 * at init time.
 */
#include "git-compat-util.h"
#include "git-zlib.h"
#ifdef USE_ZSTD
#include "wrapper.h"
#endif

#ifdef USE_ZSTD
/*
 * Declared in environment.c, controls which compression backend to use
 * for new compression operations. 0 = zlib, 1 = zstd.
 */
extern int git_compression_algorithm;

/*
 * Optional zstd dictionary for improved compression of small objects.
 * Loaded once via git_zstd_load_dictionary(), shared across all streams.
 */
static void *zstd_dict_buf;
static size_t zstd_dict_size;
static ZSTD_DDict *zstd_ddict;
static int zstd_dict_loaded;

void git_zstd_load_dictionary(const char *path)
{
	struct stat st;
	int fd;

	if (zstd_dict_loaded)
		return;
	zstd_dict_loaded = 1;

	if (!path)
		return;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return; /* no dictionary file, that's fine */

	if (fstat(fd, &st) || st.st_size == 0) {
		close(fd);
		return;
	}

	zstd_dict_size = st.st_size;
	zstd_dict_buf = xmalloc(zstd_dict_size);
	if (read_in_full(fd, zstd_dict_buf, zstd_dict_size) < 0) {
		FREE_AND_NULL(zstd_dict_buf);
		zstd_dict_size = 0;
		close(fd);
		return;
	}
	close(fd);

	/* Pre-build DDict for efficient decompression (read-only, thread-safe) */
	zstd_ddict = ZSTD_createDDict(zstd_dict_buf, zstd_dict_size);
	if (!zstd_ddict)
		die("ZSTD_createDDict: failed to create dictionary");
}
#endif

static const char *zerr_to_string(int status)
{
	switch (status) {
	case Z_MEM_ERROR:
		return "out of memory";
	case Z_VERSION_ERROR:
		return "wrong version";
	case Z_NEED_DICT:
		return "needs dictionary";
	case Z_DATA_ERROR:
		return "data stream error";
	case Z_STREAM_ERROR:
		return "stream consistency error";
	default:
		return "unknown error";
	}
}

/*
 * avail_in and avail_out in zlib are counted in uInt, which typically
 * limits the size of the buffer we can use to 4GB when interacting
 * with zlib in a single call to inflate/deflate.
 */
/* #define ZLIB_BUF_MAX ((uInt)-1) */
#define ZLIB_BUF_MAX ((uInt) 1024 * 1024 * 1024) /* 1GB */
static inline uInt zlib_buf_cap(unsigned long len)
{
	return (ZLIB_BUF_MAX < len) ? ZLIB_BUF_MAX : len;
}

static void zlib_pre_call(git_zstream *s)
{
	s->z.next_in = s->next_in;
	s->z.next_out = s->next_out;
	s->z.total_in = s->total_in;
	s->z.total_out = s->total_out;
	s->z.avail_in = zlib_buf_cap(s->avail_in);
	s->z.avail_out = zlib_buf_cap(s->avail_out);
}

static void zlib_post_call(git_zstream *s, int status)
{
	unsigned long bytes_consumed;
	unsigned long bytes_produced;

	bytes_consumed = s->z.next_in - s->next_in;
	bytes_produced = s->z.next_out - s->next_out;
	if (s->z.total_out != s->total_out + bytes_produced)
		BUG("total_out mismatch");
	/*
	 * zlib does not update total_in when it returns Z_NEED_DICT,
	 * causing a mismatch here. Skip the sanity check in that case.
	 */
	if (status != Z_NEED_DICT &&
	    s->z.total_in != s->total_in + bytes_consumed)
		BUG("total_in mismatch");

	s->total_out = s->z.total_out;
	s->total_in = s->z.total_in;
	/* zlib-ng marks `next_in` as `const`, so we have to cast it away. */
	s->next_in = (unsigned char *) s->z.next_in;
	s->next_out = s->z.next_out;
	s->avail_in -= bytes_consumed;
	s->avail_out -= bytes_produced;
}

#ifdef USE_ZSTD

/* zstd magic number: 0xFD2FB528 stored little-endian */
static int is_zstd_compressed(const unsigned char *data, unsigned long len)
{
	if (len < 4)
		return 0;
	return data[0] == 0x28 &&
	       data[1] == 0xB5 &&
	       data[2] == 0x2F &&
	       data[3] == 0xFD;
}

static void zstd_inflate_init(git_zstream *strm)
{
	strm->backend = GIT_COMPRESSION_ZSTD;
	strm->zstd_dctx = ZSTD_createDCtx();
	if (!strm->zstd_dctx)
		die("ZSTD_createDCtx: out of memory");

	/* Reference shared DDict if available (no copy, efficient) */
	if (zstd_ddict)
		ZSTD_DCtx_refDDict(strm->zstd_dctx, zstd_ddict);
}

static void zlib_inflate_init_late(git_zstream *strm)
{
	int status;

	strm->backend = GIT_COMPRESSION_ZLIB;
	zlib_pre_call(strm);
	status = inflateInit(&strm->z);
	zlib_post_call(strm, status);
	if (status == Z_OK)
		return;
	die("inflateInit: %s (%s)", zerr_to_string(status),
	    strm->z.msg ? strm->z.msg : "no message");
}

static int git_inflate_zstd(git_zstream *strm, int flush UNUSED)
{
	ZSTD_inBuffer input = { strm->next_in, strm->avail_in, 0 };
	ZSTD_outBuffer output = { strm->next_out, strm->avail_out, 0 };
	size_t ret;

	/*
	 * Unlike zlib, zstd has no trailing bytes to consume after frame
	 * completion. If the frame was already fully decompressed in a
	 * prior call, immediately signal completion.
	 */
	if (strm->zstd_inflate_done)
		return Z_STREAM_END;

	ret = ZSTD_decompressStream(strm->zstd_dctx, &output, &input);
	if (ZSTD_isError(ret))
		die("zstd inflate: %s", ZSTD_getErrorName(ret));

	strm->next_in += input.pos;
	strm->avail_in -= input.pos;
	strm->total_in += input.pos;
	strm->next_out += output.pos;
	strm->avail_out -= output.pos;
	strm->total_out += output.pos;

	if (ret == 0) {
		strm->zstd_inflate_done = 1;
		return Z_STREAM_END;
	}

	if (input.pos == 0 && output.pos == 0)
		return Z_BUF_ERROR;

	return Z_OK;
}

static int git_deflate_zstd(git_zstream *strm, int flush)
{
	ZSTD_EndDirective end_op;
	ZSTD_inBuffer input = { strm->next_in, strm->avail_in, 0 };
	ZSTD_outBuffer output = { strm->next_out, strm->avail_out, 0 };
	size_t ret;

	end_op = (flush == Z_FINISH) ? ZSTD_e_end : ZSTD_e_continue;

	ret = ZSTD_compressStream2(strm->zstd_cctx, &output, &input, end_op);
	if (ZSTD_isError(ret))
		die("zstd deflate: %s", ZSTD_getErrorName(ret));

	strm->next_in += input.pos;
	strm->avail_in -= input.pos;
	strm->total_in += input.pos;
	strm->next_out += output.pos;
	strm->avail_out -= output.pos;
	strm->total_out += output.pos;

	if (flush == Z_FINISH && ret == 0)
		return Z_STREAM_END;

	if (input.pos == 0 && output.pos == 0)
		return Z_BUF_ERROR;

	return Z_OK;
}

#endif /* USE_ZSTD */

void git_inflate_init(git_zstream *strm)
{
	int status;

#ifdef USE_ZSTD
	/*
	 * With zstd support, defer backend selection until the first
	 * git_inflate() call, where we can auto-detect the format from
	 * the magic bytes in the compressed data.
	 *
	 * Do NOT memset here — some callers (e.g. unpack_loose_header)
	 * set buffer pointers before calling init.
	 */
	strm->backend = GIT_COMPRESSION_AUTO;
	strm->zstd_dctx = NULL;
	return;
#endif

	zlib_pre_call(strm);
	status = inflateInit(&strm->z);
	zlib_post_call(strm, status);
	if (status == Z_OK)
		return;
	die("inflateInit: %s (%s)", zerr_to_string(status),
	    strm->z.msg ? strm->z.msg : "no message");
}

void git_inflate_init_gzip_only(git_zstream *strm)
{
	/*
	 * Use default 15 bits, +16 is to accept only gzip and to
	 * yield Z_DATA_ERROR when fed zlib format.
	 */
	const int windowBits = 15 + 16;
	int status;

	zlib_pre_call(strm);
	status = inflateInit2(&strm->z, windowBits);
	zlib_post_call(strm, status);
	if (status == Z_OK)
		return;
	die("inflateInit2: %s (%s)", zerr_to_string(status),
	    strm->z.msg ? strm->z.msg : "no message");
}

void git_inflate_end(git_zstream *strm)
{
	int status;

#ifdef USE_ZSTD
	if (strm->backend == GIT_COMPRESSION_ZSTD) {
		ZSTD_freeDCtx(strm->zstd_dctx);
		strm->zstd_dctx = NULL;
		return;
	}
	if (strm->backend == GIT_COMPRESSION_AUTO) {
		/* never used, nothing to clean up */
		return;
	}
#endif

	zlib_pre_call(strm);
	status = inflateEnd(&strm->z);
	zlib_post_call(strm, status);
	if (status == Z_OK)
		return;
	error("inflateEnd: %s (%s)", zerr_to_string(status),
	      strm->z.msg ? strm->z.msg : "no message");
}

int git_inflate(git_zstream *strm, int flush)
{
	int status;

#ifdef USE_ZSTD
	if (strm->backend == GIT_COMPRESSION_AUTO) {
		if (is_zstd_compressed(strm->next_in, strm->avail_in))
			zstd_inflate_init(strm);
		else
			zlib_inflate_init_late(strm);
	}
	if (strm->backend == GIT_COMPRESSION_ZSTD)
		return git_inflate_zstd(strm, flush);
#endif

	for (;;) {
		zlib_pre_call(strm);
		/* Never say Z_FINISH unless we are feeding everything */
		status = inflate(&strm->z,
				 (strm->z.avail_in != strm->avail_in)
				 ? 0 : flush);
		if (status == Z_MEM_ERROR)
			die("inflate: out of memory");
		zlib_post_call(strm, status);

		/*
		 * Let zlib work another round, while we can still
		 * make progress.
		 */
		if ((strm->avail_out && !strm->z.avail_out) &&
		    (status == Z_OK || status == Z_BUF_ERROR))
			continue;
		break;
	}

	switch (status) {
	/* Z_BUF_ERROR: normal, needs more space in the output buffer */
	case Z_BUF_ERROR:
	case Z_OK:
	case Z_STREAM_END:
		return status;
	default:
		break;
	}
	error("inflate: %s (%s)", zerr_to_string(status),
	      strm->z.msg ? strm->z.msg : "no message");
	return status;
}

unsigned long git_deflate_bound(git_zstream *strm, unsigned long size)
{
#ifdef USE_ZSTD
	if (strm->backend == GIT_COMPRESSION_ZSTD)
		return ZSTD_compressBound(size);
#endif
	return deflateBound(&strm->z, size);
}

void git_deflate_init(git_zstream *strm, int level)
{
	int status;

	memset(strm, 0, sizeof(*strm));

#ifdef USE_ZSTD
	if (git_compression_algorithm == GIT_COMPRESSION_ZSTD) {
		int zstd_level;

		strm->backend = GIT_COMPRESSION_ZSTD;
		strm->zstd_cctx = ZSTD_createCCtx();
		if (!strm->zstd_cctx)
			die("ZSTD_createCCtx: out of memory");

		/*
		 * Map zlib-style levels to zstd: Z_DEFAULT_COMPRESSION (-1)
		 * and Z_BEST_SPEED (1) both map to zstd level 1 for fast
		 * loose object writes. Otherwise pass the level through
		 * directly — zstd accepts 1-22 (and negative for fast mode).
		 */
		if (level == Z_DEFAULT_COMPRESSION || level == Z_BEST_SPEED)
			zstd_level = 3;
		else if (level < 1)
			zstd_level = 1;
		else
			zstd_level = level;

		ZSTD_CCtx_setParameter(strm->zstd_cctx,
					ZSTD_c_compressionLevel, zstd_level);
		ZSTD_CCtx_setParameter(strm->zstd_cctx,
					ZSTD_c_checksumFlag, 1);

		/* Load dictionary if available (copies into CCtx) */
		if (zstd_dict_buf)
			ZSTD_CCtx_loadDictionary(strm->zstd_cctx,
						  zstd_dict_buf,
						  zstd_dict_size);
		return;
	}
#endif

	strm->backend = GIT_COMPRESSION_ZLIB;
	zlib_pre_call(strm);
	status = deflateInit(&strm->z, level);
	zlib_post_call(strm, status);
	if (status == Z_OK)
		return;
	die("deflateInit: %s (%s)", zerr_to_string(status),
	    strm->z.msg ? strm->z.msg : "no message");
}

static void do_git_deflate_init(git_zstream *strm, int level, int windowBits)
{
	int status;

	memset(strm, 0, sizeof(*strm));
	zlib_pre_call(strm);
	status = deflateInit2(&strm->z, level,
				  Z_DEFLATED, windowBits,
				  8, Z_DEFAULT_STRATEGY);
	zlib_post_call(strm, status);
	if (status == Z_OK)
		return;
	die("deflateInit2: %s (%s)", zerr_to_string(status),
	    strm->z.msg ? strm->z.msg : "no message");
}

void git_deflate_init_gzip(git_zstream *strm, int level)
{
	/*
	 * Use default 15 bits, +16 is to generate gzip header/trailer
	 * instead of the zlib wrapper.
	 */
	do_git_deflate_init(strm, level, 15 + 16);
}

void git_deflate_init_raw(git_zstream *strm, int level)
{
	/*
	 * Use default 15 bits, negate the value to get raw compressed
	 * data without zlib header and trailer.
	 */
	do_git_deflate_init(strm, level, -15);
}

int git_deflate_abort(git_zstream *strm)
{
	int status;

#ifdef USE_ZSTD
	if (strm->backend == GIT_COMPRESSION_ZSTD) {
		ZSTD_freeCCtx(strm->zstd_cctx);
		strm->zstd_cctx = NULL;
		return Z_OK;
	}
#endif

	zlib_pre_call(strm);
	status = deflateEnd(&strm->z);
	zlib_post_call(strm, status);
	return status;
}

void git_deflate_end(git_zstream *strm)
{
	int status = git_deflate_abort(strm);

	if (status == Z_OK)
		return;
	error("deflateEnd: %s (%s)", zerr_to_string(status),
	      strm->z.msg ? strm->z.msg : "no message");
}

int git_deflate_end_gently(git_zstream *strm)
{
	int status;

#ifdef USE_ZSTD
	if (strm->backend == GIT_COMPRESSION_ZSTD) {
		ZSTD_freeCCtx(strm->zstd_cctx);
		strm->zstd_cctx = NULL;
		return Z_OK;
	}
#endif

	zlib_pre_call(strm);
	status = deflateEnd(&strm->z);
	zlib_post_call(strm, status);
	return status;
}

int git_deflate(git_zstream *strm, int flush)
{
	int status;

#ifdef USE_ZSTD
	if (strm->backend == GIT_COMPRESSION_ZSTD)
		return git_deflate_zstd(strm, flush);
#endif

	for (;;) {
		zlib_pre_call(strm);

		/* Never say Z_FINISH unless we are feeding everything */
		status = deflate(&strm->z,
				 (strm->z.avail_in != strm->avail_in)
				 ? 0 : flush);
		if (status == Z_MEM_ERROR)
			die("deflate: out of memory");
		zlib_post_call(strm, status);

		/*
		 * Let zlib work another round, while we can still
		 * make progress.
		 */
		if ((strm->avail_out && !strm->z.avail_out) &&
		    (status == Z_OK || status == Z_BUF_ERROR))
			continue;
		break;
	}

	switch (status) {
	/* Z_BUF_ERROR: normal, needs more space in the output buffer */
	case Z_BUF_ERROR:
	case Z_OK:
	case Z_STREAM_END:
		return status;
	default:
		break;
	}
	error("deflate: %s (%s)", zerr_to_string(status),
	      strm->z.msg ? strm->z.msg : "no message");
	return status;
}
