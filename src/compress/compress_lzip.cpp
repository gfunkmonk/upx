/* compress_lzip.cpp --

   This file is part of the UPX executable compressor.

   Copyright (C) Markus Franz Xaver Johannes Oberhumer
   All Rights Reserved.

   UPX and the UCL library are free software; you can redistribute them
   and/or modify them under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.
   If not, write to the Free Software Foundation, Inc.,
   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

   Markus F.X.J. Oberhumer
   <markus@oberhumer.com>
 */

#include "../util/system_headers.h"
#if WITH_LZIP
#include <lzlib.h>
#endif
#include "../conf.h"

void lzip_compress_config_t::reset() noexcept { mem_clear(this); }

#if WITH_LZIP
#include "compress.h"
#include "../util/membuffer.h"

/* Mapping from UPX compression levels 1..10 to lzip dictionary size and
   match length limit parameters (same as lzip's own level mapping). */
struct lzip_options {
    int dictionary_size;
    int match_len_limit;
};

static const struct lzip_options lzip_level_map[] = {
    {   65535,  16 }, /* level 1  (65535 selects fast encoder) */
    { 1 << 20,   5 }, /* level 2  */
    { 3 << 19,   6 }, /* level 3  */
    { 1 << 21,   8 }, /* level 4  */
    { 3 << 20,  12 }, /* level 5  */
    { 1 << 22,  20 }, /* level 6  */
    { 1 << 23,  36 }, /* level 7  */
    { 1 << 24,  68 }, /* level 8  */
    { 3 << 23, 132 }, /* level 9  */
    { 1 << 25, 273 }, /* level 10 */
};

static int convert_errno_from_lzip(enum LZ_Errno le) {
    switch (le) {
    case LZ_ok:
        return UPX_E_OK;
    case LZ_mem_error:
        return UPX_E_OUT_OF_MEMORY;
    case LZ_header_error:
    case LZ_data_error:
    case LZ_unexpected_eof:
        return UPX_E_INPUT_OVERRUN;
    default:
        break;
    }
    return UPX_E_ERROR;
}

/*************************************************************************
//
**************************************************************************/

int upx_lzip_compress(const upx_bytep src, unsigned src_len, upx_bytep dst, unsigned *dst_len,
                      upx_callback_t *cb_parm, int method, int level,
                      const upx_compress_config_t *cconf_parm, upx_compress_result_t *cresult) {
    assert(method == M_LZIP);
    assert(level >= 1 && level <= 10);
    assert(cresult != nullptr);
    UNUSED(cb_parm);
    UNUSED(cconf_parm);
    int r = UPX_E_ERROR;
    lzip_compress_result_t *const res = &cresult->result_lzip;
    res->reset();

    const struct lzip_options *const opts = &lzip_level_map[level - 1];
    int dict_size = opts->dictionary_size;
    if (dict_size > (int) src_len && level != 1)
        dict_size = (int) src_len;
    if (dict_size < LZ_min_dictionary_size())
        dict_size = LZ_min_dictionary_size();

    struct LZ_Encoder *const encoder =
        LZ_compress_open(dict_size, opts->match_len_limit, (unsigned long long) -1);
    if (!encoder || LZ_compress_errno(encoder) != LZ_ok) {
        LZ_compress_close(encoder);
        return UPX_E_OUT_OF_MEMORY;
    }

    unsigned inpos = 0;
    unsigned outpos = 0;
    bool error = false;
    while (true) {
        int ret = LZ_compress_write(encoder, src + inpos,
                                    (int) upx::min(src_len - inpos, (unsigned) INT_MAX));
        if (ret < 0) {
            error = true;
            break;
        }
        inpos += (unsigned) ret;
        if (inpos >= src_len)
            LZ_compress_finish(encoder);
        ret = LZ_compress_read(encoder, dst + outpos,
                               (int) upx::min(*dst_len - outpos, (unsigned) INT_MAX));
        if (ret < 0) {
            error = true;
            break;
        }
        outpos += (unsigned) ret;
        if (LZ_compress_finished(encoder) == 1)
            break;
        if (outpos >= *dst_len) {
            r = UPX_E_OUTPUT_OVERRUN;
            error = true;
            break;
        }
    }

    if (!error && LZ_compress_close(encoder) < 0)
        error = true;
    else if (error)
        LZ_compress_close(encoder);

    if (!error) {
        *dst_len = outpos;
        r = UPX_E_OK;
    }
    return r;
}

/*************************************************************************
//
**************************************************************************/

int upx_lzip_decompress(const upx_bytep src, unsigned src_len, upx_bytep dst, unsigned *dst_len,
                        int method, const upx_compress_result_t *cresult) {
    assert(method == M_LZIP);
    UNUSED(method);
    UNUSED(cresult);
    int r = UPX_E_ERROR;

    struct LZ_Decoder *const decoder = LZ_decompress_open();
    if (!decoder || LZ_decompress_errno(decoder) != LZ_ok) {
        LZ_decompress_close(decoder);
        return UPX_E_OUT_OF_MEMORY;
    }

    unsigned inpos = 0;
    unsigned outpos = 0;
    bool overflow = false;
    bool error = false;
    upx_byte overflow_buf[1];
    while (true) {
        int ret = LZ_decompress_write(decoder, src + inpos,
                                     (int) upx::min(src_len - inpos, (unsigned) INT_MAX));
        if (ret < 0) {
            error = true;
            break;
        }
        inpos += (unsigned) ret;
        if (inpos >= src_len)
            LZ_decompress_finish(decoder);
        upx_bytep out_ptr = dst + outpos;
        unsigned out_avail = *dst_len - outpos;
        if (out_avail == 0) {
            out_ptr = overflow_buf;
            out_avail = sizeof(overflow_buf);
        }
        ret = LZ_decompress_read(decoder, out_ptr, (int) upx::min(out_avail, (unsigned) INT_MAX));
        if (ret < 0) {
            r = convert_errno_from_lzip(LZ_decompress_errno(decoder));
            error = true;
            break;
        }
        if (out_ptr == overflow_buf && ret > 0)
            overflow = true;
        else
            outpos += (unsigned) ret;
        if (LZ_decompress_finished(decoder) == 1)
            break;
        if (inpos >= src_len && ret == 0 && LZ_decompress_finished(decoder) != 1) {
            r = UPX_E_INPUT_OVERRUN;
            error = true;
            break;
        }
    }

    if (!overflow && outpos > *dst_len)
        overflow = true;

    if (!error) {
        if (overflow)
            r = UPX_E_OUTPUT_OVERRUN;
        else {
            *dst_len = outpos;
            r = UPX_E_OK;
        }
    }
    LZ_decompress_close(decoder);
    return r;
}

/*************************************************************************
// test_overlap - see <ucl/ucl.h> for semantics
**************************************************************************/

int upx_lzip_test_overlap(const upx_bytep buf, const upx_bytep tbuf, unsigned src_off,
                          unsigned src_len, unsigned *dst_len, int method,
                          const upx_compress_result_t *cresult) {
    assert(method == M_LZIP);

    MemBuffer b(src_off + src_len);
    memcpy(b + src_off, buf + src_off, src_len);
    unsigned saved_dst_len = *dst_len;
    int r = upx_lzip_decompress(raw_index_bytes(b, src_off, src_len), src_len,
                                raw_bytes(b, *dst_len), dst_len, method, cresult);
    if (r != UPX_E_OK)
        return r;
    if (*dst_len != saved_dst_len)
        return UPX_E_ERROR;
    // NOTE: there is a very tiny possibility that decompression has
    //   succeeded but the data is not restored correctly because of
    //   in-place buffer overlapping, so we use an extra memcmp().
    if (tbuf != nullptr && memcmp(tbuf, b, *dst_len) != 0)
        return UPX_E_ERROR;
    return UPX_E_OK;
}

/*************************************************************************
// misc
**************************************************************************/

int upx_lzip_init(void) {
    if (LZ_api_version() < LZ_API_VERSION)
        return -1;
    return 0;
}

const char *upx_lzip_version_string(void) { return LZ_version(); }

/*************************************************************************
// doctest checks
**************************************************************************/

#if DEBUG && !defined(DOCTEST_CONFIG_DISABLE) && 1

static bool check_lzip(const int method, const int level, const unsigned expected_c_len) {
    const unsigned u_len = 16384;
    const unsigned c_extra = 4096;
    MemBuffer u_buf, c_buf, d_buf;
    unsigned c_len, d_len;
    upx_compress_result_t cresult;
    int r;

    u_buf.alloc(u_len);
    memset(u_buf, 0, u_len);
    c_buf.allocForCompression(u_len, c_extra);
    d_buf.allocForDecompression(u_len);

    c_len = c_buf.getSize() - c_extra;
    r = upx_lzip_compress(raw_bytes(u_buf, u_len), u_len, raw_index_bytes(c_buf, c_extra, c_len),
                          &c_len, nullptr, method, level, NULL_cconf, &cresult);
    if (r != 0 || c_len != expected_c_len)
        return false;

    d_len = d_buf.getSize();
    r = upx_lzip_decompress(raw_index_bytes(c_buf, c_extra, c_len), c_len, raw_bytes(d_buf, d_len),
                            &d_len, method, nullptr);
    if (r != 0 || d_len != u_len || memcmp(u_buf, d_buf, u_len) != 0)
        return false;

    d_len = u_len - 1;
    r = upx_lzip_decompress(raw_index_bytes(c_buf, c_extra, c_len), c_len, raw_bytes(d_buf, d_len),
                            &d_len, method, nullptr);
    if (r == 0)
        return false;

    return true;
}

TEST_CASE("compress_lzip") { CHECK(check_lzip(M_LZIP, 1, 49)); }

#endif // DEBUG

TEST_CASE("upx_lzip_decompress") {
    const byte *c_data;
    byte d_buf[16];
    unsigned d_len;
    int r;

    // 16 zero bytes compressed with lzip level 1
    // generated via: python3 -c "import sys; sys.stdout.buffer.write(b'\x00'*16)" | lzip -1 | xxd -i
    c_data = (const byte *) "\x4c\x5a\x49\x50\x01\x0c\x00\x24\x19\x49\x98\x3f\x00\x00\x00\x00"
                            "\x00\x00\x00\x00\x00\x00\x10\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                            "\x5a\xd2\xe5\x75\x10\x00\x00\x00\x00\x00\x00\x00\x31\x00\x00\x00"
                            "\x00\x00\x00\x00";
    d_len = 16;
    r = upx_lzip_decompress(c_data, 52, d_buf, &d_len, M_LZIP, nullptr);
    CHECK((r == 0 && d_len == 16));
    d_len = 15;
    r = upx_lzip_decompress(c_data, 52, d_buf, &d_len, M_LZIP, nullptr);
    CHECK(r == UPX_E_OUTPUT_OVERRUN);
    UNUSED(r);
}

#endif // WITH_LZIP

/* vim:set ts=4 sw=4 et: */
