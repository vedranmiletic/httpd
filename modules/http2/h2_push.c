/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include <assert.h>
#include <stdio.h>

#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_hash.h>
#include <apr_time.h>

#ifdef H2_OPENSSL
#include <openssl/evp.h>
#endif

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_protocol.h>

#include "h2_private.h"
#include "h2_protocol.h"
#include "h2_util.h"
#include "h2_push.h"
#include "h2_request.h"
#include "h2_session.h"
#include "h2_stream.h"

/*******************************************************************************
 * link header handling 
 ******************************************************************************/

static const char *policy_str(h2_push_policy policy)
{
    switch (policy) {
        case H2_PUSH_NONE:
            return "none";
        case H2_PUSH_FAST_LOAD:
            return "fast-load";
        case H2_PUSH_HEAD:
            return "head";
        default:
            return "default";
    }
}

typedef struct {
    const h2_request *req;
    apr_uint32_t push_policy;
    apr_pool_t *pool;
    apr_array_header_t *pushes;
    const char *s;
    size_t slen;
    size_t i;
    
    const char *link;
    apr_table_t *params;
    char b[4096];
} link_ctx;

static int attr_char(char c) 
{
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '&':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return 1;
        default:
            return apr_isalnum(c);
    }
}

static int ptoken_char(char c) 
{
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case '-':
        case '.':
        case '/':
        case ':':
        case '<':
        case '=':
        case '>':
        case '?':
        case '@':
        case '[':
        case ']':
        case '^':
        case '_':
        case '`':
        case '{':
        case '|':
        case '}':
        case '~':
            return 1;
        default:
            return apr_isalnum(c);
    }
}

static int skip_ws(link_ctx *ctx)
{
    char c;
    while (ctx->i < ctx->slen 
           && (((c = ctx->s[ctx->i]) == ' ') || (c == '\t'))) {
        ++ctx->i;
    }
    return (ctx->i < ctx->slen);
}

static int find_chr(link_ctx *ctx, char c, size_t *pidx)
{
    size_t j;
    for (j = ctx->i; j < ctx->slen; ++j) {
        if (ctx->s[j] == c) {
            *pidx = j;
            return 1;
        }
    } 
    return 0;
}

static int read_chr(link_ctx *ctx, char c)
{
    if (ctx->i < ctx->slen && ctx->s[ctx->i] == c) {
        ++ctx->i;
        return 1;
    }
    return 0;
}

static char *mk_str(link_ctx *ctx, size_t end) 
{
    if (ctx->i < end) {
        return apr_pstrndup(ctx->pool, ctx->s + ctx->i, end - ctx->i);
    }
    return (char*)"";
}

static int read_qstring(link_ctx *ctx, const char **ps)
{
    if (skip_ws(ctx) && read_chr(ctx, '\"')) {
        size_t end;
        if (find_chr(ctx, '\"', &end)) {
            *ps = mk_str(ctx, end);
            ctx->i = end + 1;
            return 1;
        }
    }
    return 0;
}

static int read_ptoken(link_ctx *ctx, const char **ps)
{
    if (skip_ws(ctx)) {
        size_t i;
        for (i = ctx->i; i < ctx->slen && ptoken_char(ctx->s[i]); ++i) {
            /* nop */
        }
        if (i > ctx->i) {
            *ps = mk_str(ctx, i);
            ctx->i = i;
            return 1;
        }
    }
    return 0;
}


static int read_link(link_ctx *ctx)
{
    if (skip_ws(ctx) && read_chr(ctx, '<')) {
        size_t end;
        if (find_chr(ctx, '>', &end)) {
            ctx->link = mk_str(ctx, end);
            ctx->i = end + 1;
            return 1;
        }
    }
    return 0;
}

static int read_pname(link_ctx *ctx, const char **pname)
{
    if (skip_ws(ctx)) {
        size_t i;
        for (i = ctx->i; i < ctx->slen && attr_char(ctx->s[i]); ++i) {
            /* nop */
        }
        if (i > ctx->i) {
            *pname = mk_str(ctx, i);
            ctx->i = i;
            return 1;
        }
    }
    return 0;
}

static int read_pvalue(link_ctx *ctx, const char **pvalue)
{
    if (skip_ws(ctx) && read_chr(ctx, '=')) {
        if (read_qstring(ctx, pvalue) || read_ptoken(ctx, pvalue)) {
            return 1;
        }
    }
    return 0;
}

static int read_param(link_ctx *ctx)
{
    if (skip_ws(ctx) && read_chr(ctx, ';')) {
        const char *name, *value = "";
        if (read_pname(ctx, &name)) {
            read_pvalue(ctx, &value); /* value is optional */
            apr_table_setn(ctx->params, name, value);
            return 1;
        }
    }
    return 0;
}

static int read_sep(link_ctx *ctx)
{
    if (skip_ws(ctx) && read_chr(ctx, ',')) {
        return 1;
    }
    return 0;
}

static void init_params(link_ctx *ctx) 
{
    if (!ctx->params) {
        ctx->params = apr_table_make(ctx->pool, 5);
    }
    else {
        apr_table_clear(ctx->params);
    }
}

static int same_authority(const h2_request *req, const apr_uri_t *uri)
{
    if (uri->scheme != NULL && strcmp(uri->scheme, req->scheme)) {
        return 0;
    }
    if (uri->hostinfo != NULL && strcmp(uri->hostinfo, req->authority)) {
        return 0;
    }
    return 1;
}

static int set_push_header(void *ctx, const char *key, const char *value) 
{
    size_t klen = strlen(key);
    if (H2_HD_MATCH_LIT("User-Agent", key, klen)
        || H2_HD_MATCH_LIT("Accept", key, klen)
        || H2_HD_MATCH_LIT("Accept-Encoding", key, klen)
        || H2_HD_MATCH_LIT("Accept-Language", key, klen)
        || H2_HD_MATCH_LIT("Cache-Control", key, klen)) {
        apr_table_setn(ctx, key, value);
    }
    return 1;
}

static int has_param(link_ctx *ctx, const char *param)
{
    const char *p = apr_table_get(ctx->params, param);
    return !!p;
}

static int has_relation(link_ctx *ctx, const char *rel)
{
    const char *s, *val = apr_table_get(ctx->params, "rel");
    if (val) {
        if (!strcmp(rel, val)) {
            return 1;
        }
        s = ap_strstr_c(val, rel);
        if (s && (s == val || s[-1] == ' ')) {
            s += strlen(rel);
            if (!*s || *s == ' ') {
                return 1;
            }
        }
    }
    return 0;
}

static int add_push(link_ctx *ctx)
{
    /* so, we have read a Link header and need to decide
     * if we transform it into a push.
     */
    if (has_relation(ctx, "preload") && !has_param(ctx, "nopush")) {
        apr_uri_t uri;
        if (apr_uri_parse(ctx->pool, ctx->link, &uri) == APR_SUCCESS) {
            if (uri.path && same_authority(ctx->req, &uri)) {
                char *path;
                const char *method;
                apr_table_t *headers;
                h2_request *req;
                h2_push *push;
                
                /* We only want to generate pushes for resources in the
                 * same authority than the original request.
                 * icing: i think that is wise, otherwise we really need to
                 * check that the vhost/server is available and uses the same
                 * TLS (if any) parameters.
                 */
                path = apr_uri_unparse(ctx->pool, &uri, APR_URI_UNP_OMITSITEPART);
                push = apr_pcalloc(ctx->pool, sizeof(*push));
                switch (ctx->push_policy) {
                    case H2_PUSH_HEAD:
                        method = "HEAD";
                        break;
                    default:
                        method = "GET";
                        break;
                }
                headers = apr_table_make(ctx->pool, 5);
                apr_table_do(set_push_header, headers, ctx->req->headers, NULL);
                req = h2_request_create(0, ctx->pool, method, ctx->req->scheme,
                                        ctx->req->authority, path, headers);
                /* atm, we do not push on pushes */
                h2_request_end_headers(req, ctx->pool, 1, 0);
                push->req = req;
                if (has_param(ctx, "critical")) {
                    h2_priority *prio = apr_pcalloc(ctx->pool, sizeof(*prio));
                    prio->dependency = H2_DEPENDANT_BEFORE;
                    push->priority = prio;
                }
                if (!ctx->pushes) {
                    ctx->pushes = apr_array_make(ctx->pool, 5, sizeof(h2_push*));
                }
                APR_ARRAY_PUSH(ctx->pushes, h2_push*) = push;
            }
        }
    }
    return 0;
}

static void inspect_link(link_ctx *ctx, const char *s, size_t slen)
{
    /* RFC 5988 <https://tools.ietf.org/html/rfc5988#section-6.2.1>
      Link           = "Link" ":" #link-value
      link-value     = "<" URI-Reference ">" *( ";" link-param )
      link-param     = ( ( "rel" "=" relation-types )
                     | ( "anchor" "=" <"> URI-Reference <"> )
                     | ( "rev" "=" relation-types )
                     | ( "hreflang" "=" Language-Tag )
                     | ( "media" "=" ( MediaDesc | ( <"> MediaDesc <"> ) ) )
                     | ( "title" "=" quoted-string )
                     | ( "title*" "=" ext-value )
                     | ( "type" "=" ( media-type | quoted-mt ) )
                     | ( link-extension ) )
      link-extension = ( parmname [ "=" ( ptoken | quoted-string ) ] )
                     | ( ext-name-star "=" ext-value )
      ext-name-star  = parmname "*" ; reserved for RFC2231-profiled
                                    ; extensions.  Whitespace NOT
                                    ; allowed in between.
      ptoken         = 1*ptokenchar
      ptokenchar     = "!" | "#" | "$" | "%" | "&" | "'" | "("
                     | ")" | "*" | "+" | "-" | "." | "/" | DIGIT
                     | ":" | "<" | "=" | ">" | "?" | "@" | ALPHA
                     | "[" | "]" | "^" | "_" | "`" | "{" | "|"
                     | "}" | "~"
      media-type     = type-name "/" subtype-name
      quoted-mt      = <"> media-type <">
      relation-types = relation-type
                     | <"> relation-type *( 1*SP relation-type ) <">
      relation-type  = reg-rel-type | ext-rel-type
      reg-rel-type   = LOALPHA *( LOALPHA | DIGIT | "." | "-" )
      ext-rel-type   = URI
      
      and from <https://tools.ietf.org/html/rfc5987>
      parmname      = 1*attr-char
      attr-char     = ALPHA / DIGIT
                       / "!" / "#" / "$" / "&" / "+" / "-" / "."
                       / "^" / "_" / "`" / "|" / "~"
     */

     ctx->s = s;
     ctx->slen = slen;
     ctx->i = 0;
     
     while (read_link(ctx)) {
        init_params(ctx);
        while (read_param(ctx)) {
            /* nop */
        }
        add_push(ctx);
        if (!read_sep(ctx)) {
            break;
        }
     }
}

static int head_iter(void *ctx, const char *key, const char *value) 
{
    if (!apr_strnatcasecmp("link", key)) {
        inspect_link(ctx, value, strlen(value));
    }
    return 1;
}

#if AP_HAS_RESPONSE_BUCKETS
apr_array_header_t *h2_push_collect(apr_pool_t *p,
                                    const struct h2_request *req,
                                    apr_uint32_t push_policy,
                                    const ap_bucket_response *res)
#else
apr_array_header_t *h2_push_collect(apr_pool_t *p,
                                    const struct h2_request *req,
                                    apr_uint32_t push_policy,
                                    const struct h2_headers *res)
#endif
{
    if (req && push_policy != H2_PUSH_NONE) {
        /* Collect push candidates from the request/response pair.
         * 
         * One source for pushes are "rel=preload" link headers
         * in the response.
         * 
         * TODO: This may be extended in the future by hooks or callbacks
         * where other modules can provide push information directly.
         */
        if (res->headers) {
            link_ctx ctx;
            
            memset(&ctx, 0, sizeof(ctx));
            ctx.req = req;
            ctx.push_policy = push_policy;
            ctx.pool = p;
            
            apr_table_do(head_iter, &ctx, res->headers, NULL);
            if (ctx.pushes) {
                apr_table_setn(res->headers, "push-policy", 
                               policy_str(push_policy));
            }
            return ctx.pushes;
        }
    }
    return NULL;
}

#define GCSLOG_LEVEL   APLOG_TRACE1

typedef struct h2_push_diary_entry {
    apr_uint64_t hash;
} h2_push_diary_entry;


#ifdef H2_OPENSSL
static void sha256_update(EVP_MD_CTX *ctx, const char *s)
{
    EVP_DigestUpdate(ctx, s, strlen(s));
}

static void calc_sha256_hash(h2_push_diary *diary, apr_uint64_t *phash, h2_push *push) 
{
    EVP_MD_CTX *md;
    apr_uint64_t val;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned len, i;

    md = EVP_MD_CTX_create();
    ap_assert(md != NULL);

    i = EVP_DigestInit_ex(md, EVP_sha256(), NULL);
    ap_assert(i == 1);
    sha256_update(md, push->req->scheme);
    sha256_update(md, "://");
    sha256_update(md, push->req->authority);
    sha256_update(md, push->req->path);
    EVP_DigestFinal(md, hash, &len);

    val = 0;
    for (i = 0; i != len; ++i)
        val = val * 256 + hash[i];
    *phash = val >> (64 - diary->mask_bits);
}
#endif


static unsigned int val_apr_hash(const char *str) 
{
    apr_ssize_t len = (apr_ssize_t)strlen(str);
    return apr_hashfunc_default(str, &len);
}

static void calc_apr_hash(h2_push_diary *diary, apr_uint64_t *phash, h2_push *push) 
{
    apr_uint64_t val;
    (void)diary;
#if APR_UINT64_MAX > UINT_MAX
    val = ((apr_uint64_t)(val_apr_hash(push->req->scheme)) << 32);
    val ^= ((apr_uint64_t)(val_apr_hash(push->req->authority)) << 16);
    val ^= val_apr_hash(push->req->path);
#else
    val = val_apr_hash(push->req->scheme);
    val ^= val_apr_hash(push->req->authority);
    val ^= val_apr_hash(push->req->path);
#endif
    *phash = val;
}

static apr_int32_t ceil_power_of_2(apr_int32_t n)
{
    if (n <= 2) return 2;
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return ++n;
}

static h2_push_diary *diary_create(apr_pool_t *p, h2_push_digest_type dtype, 
                                   int N)
{
    h2_push_diary *diary = NULL;
    
    if (N > 0) {
        diary = apr_pcalloc(p, sizeof(*diary));
        
        diary->NMax        = ceil_power_of_2(N);
        diary->N           = diary->NMax;
        /* the mask we use in value comparison depends on where we got
         * the values from. If we calculate them ourselves, we can use
         * the full 64 bits.
         * If we set the diary via a compressed golomb set, we have less
         * relevant bits and need to use a smaller mask. */
        diary->mask_bits   = 64;
        /* grows by doubling, start with a power of 2 */
        diary->entries     = apr_array_make(p, 16, sizeof(h2_push_diary_entry));
        
        switch (dtype) {
#ifdef H2_OPENSSL
            case H2_PUSH_DIGEST_SHA256:
                diary->dtype       = H2_PUSH_DIGEST_SHA256;
                diary->dcalc       = calc_sha256_hash;
                break;
#endif /* ifdef H2_OPENSSL */
            default:
                diary->dtype       = H2_PUSH_DIGEST_APR_HASH;
                diary->dcalc       = calc_apr_hash;
                break;
        }
    }
    
    return diary;
}

h2_push_diary *h2_push_diary_create(apr_pool_t *p, int N)
{
    return diary_create(p, H2_PUSH_DIGEST_SHA256, N);
}

static int h2_push_diary_find(h2_push_diary *diary, apr_uint64_t hash)
{
    if (diary) {
        h2_push_diary_entry *e;
        int i;

        /* search from the end, where the last accessed digests are */
        for (i = diary->entries->nelts-1; i >= 0; --i) {
            e = &APR_ARRAY_IDX(diary->entries, i, h2_push_diary_entry);
            if (e->hash == hash) {
                return i;
            }
        }
    }
    return -1;
}

static void move_to_last(h2_push_diary *diary, apr_size_t idx)
{
    h2_push_diary_entry *entries = (h2_push_diary_entry*)diary->entries->elts;
    h2_push_diary_entry e;
    apr_size_t lastidx;
    
    /* Move an existing entry to the last place */
    if (diary->entries->nelts <= 0)
        return;

    /* move entry[idx] to the end */
    lastidx = diary->entries->nelts - 1;
    if (idx < lastidx) {
        e =  entries[idx];
        memmove(entries+idx, entries+idx+1, sizeof(h2_push_diary_entry) * (lastidx - idx));
        entries[lastidx] = e;
    }
}

static void remove_first(h2_push_diary *diary)
{
    h2_push_diary_entry *entries = (h2_push_diary_entry*)diary->entries->elts;
    int lastidx;
    
    /* move remaining entries to index 0 */
    lastidx = diary->entries->nelts - 1;
    if (lastidx > 0) {
        --diary->entries->nelts;
        memmove(entries, entries+1, sizeof(h2_push_diary_entry) * diary->entries->nelts);
    }
}

static void h2_push_diary_append(h2_push_diary *diary, h2_push_diary_entry *e)
{
    while (diary->entries->nelts >= diary->N) {
        remove_first(diary);
    }
    /* append a new diary entry at the end */
    APR_ARRAY_PUSH(diary->entries, h2_push_diary_entry) = *e;
    /* Intentional no APLOGNO */
    ap_log_perror(APLOG_MARK, GCSLOG_LEVEL, 0, diary->entries->pool,
                  "push_diary_append: %"APR_UINT64_T_HEX_FMT, e->hash);
}

apr_array_header_t *h2_push_diary_update(h2_session *session, apr_array_header_t *pushes)
{
    apr_array_header_t *npushes = pushes;
    h2_push_diary_entry e;
    int i, idx;
    
    if (session->push_diary && pushes) {
        npushes = NULL;
        
        for (i = 0; i < pushes->nelts; ++i) {
            h2_push *push;
            
            push = APR_ARRAY_IDX(pushes, i, h2_push*);
            session->push_diary->dcalc(session->push_diary, &e.hash, push);
            idx = h2_push_diary_find(session->push_diary, e.hash);
            if (idx >= 0) {
                /* Intentional no APLOGNO */
                ap_log_cerror(APLOG_MARK, GCSLOG_LEVEL, 0, session->c1,
                              "push_diary_update: already there PUSH %s", push->req->path);
                move_to_last(session->push_diary, (apr_size_t)idx);
            }
            else {
                /* Intentional no APLOGNO */
                ap_log_cerror(APLOG_MARK, GCSLOG_LEVEL, 0, session->c1,
                              "push_diary_update: adding PUSH %s", push->req->path);
                if (!npushes) {
                    npushes = apr_array_make(pushes->pool, 5, sizeof(h2_push_diary_entry*));
                }
                APR_ARRAY_PUSH(npushes, h2_push*) = push;
                h2_push_diary_append(session->push_diary, &e);
            }
        }
    }
    return npushes;
}
    
#if AP_HAS_RESPONSE_BUCKETS
apr_array_header_t *h2_push_collect_update(struct h2_stream *stream,
                                           const struct h2_request *req,
                                           const ap_bucket_response *res)
#else
apr_array_header_t *h2_push_collect_update(struct h2_stream *stream,
                                           const struct h2_request *req,
                                           const struct h2_headers *res)
#endif
{
    apr_array_header_t *pushes;
    
    pushes = h2_push_collect(stream->pool, req, stream->push_policy, res);
    return h2_push_diary_update(stream->session, pushes);
}

typedef struct {
    h2_push_diary *diary;
    unsigned char log2p;
    int mask_bits;
    int delta_bits;
    int fixed_bits;
    apr_uint64_t fixed_mask;
    apr_pool_t *pool;
    unsigned char *data;
    apr_size_t datalen;
    apr_size_t offset;
    unsigned int bit;
    apr_uint64_t last;
} gset_encoder;

static int cmp_puint64(const void *p1, const void *p2)
{
    const apr_uint64_t *pu1 = p1, *pu2 = p2;
    return (*pu1 > *pu2)? 1 : ((*pu1 == *pu2)? 0 : -1);
}

/* in golomb bit stream encoding, bit 0 is the 8th of the first char, or
 * more generally: 
 *      char(bit/8) & cbit_mask[(bit % 8)]
 */
static unsigned char cbit_mask[] = {
    0x80u,
    0x40u,
    0x20u,
    0x10u,
    0x08u,
    0x04u,
    0x02u,
    0x01u,
};

static apr_status_t gset_encode_bit(gset_encoder *encoder, int bit)
{
    if (++encoder->bit >= 8) {
        if (++encoder->offset >= encoder->datalen) {
            apr_size_t nlen = encoder->datalen*2;
            unsigned char *ndata = apr_pcalloc(encoder->pool, nlen);
            if (!ndata) {
                return APR_ENOMEM;
            }
            memcpy(ndata, encoder->data, encoder->datalen);
            encoder->data = ndata;
            encoder->datalen = nlen;
        }
        encoder->bit = 0;
        encoder->data[encoder->offset] = 0xffu;
    }
    if (!bit) {
        encoder->data[encoder->offset] &= ~cbit_mask[encoder->bit];
    }
    return APR_SUCCESS;
}

static apr_status_t gset_encode_next(gset_encoder *encoder, apr_uint64_t pval)
{
    apr_uint64_t delta, flex_bits;
    apr_status_t status = APR_SUCCESS;
    int i;
    
    delta = pval - encoder->last;
    encoder->last = pval;
    flex_bits = (delta >> encoder->fixed_bits);
    /* Intentional no APLOGNO */
    ap_log_perror(APLOG_MARK, GCSLOG_LEVEL, 0, encoder->pool,
                  "h2_push_diary_enc: val=%"APR_UINT64_T_HEX_FMT", delta=%"
                  APR_UINT64_T_HEX_FMT" flex_bits=%"APR_UINT64_T_FMT", "
                  ", fixed_bits=%d, fixed_val=%"APR_UINT64_T_HEX_FMT, 
                  pval, delta, flex_bits, encoder->fixed_bits, delta&encoder->fixed_mask);
    for (; flex_bits != 0; --flex_bits) {
        status = gset_encode_bit(encoder, 1);
        if (status != APR_SUCCESS) {
            return status;
        }
    }
    status = gset_encode_bit(encoder, 0);
    if (status != APR_SUCCESS) {
        return status;
    }

    for (i = encoder->fixed_bits-1; i >= 0; --i) {
        status = gset_encode_bit(encoder, (delta >> i) & 1);
        if (status != APR_SUCCESS) {
            return status;
        }
    }
    return APR_SUCCESS;
}

/**
 * Get a cache digest as described in 
 * https://datatracker.ietf.org/doc/draft-kazuho-h2-cache-digest/
 * from the contents of the push diary.
 * 
 * @param diary the diary to calculdate the digest from
 * @param p the pool to use
 * @param pdata on successful return, the binary cache digest
 * @param plen on successful return, the length of the binary data
 */
apr_status_t h2_push_diary_digest_get(h2_push_diary *diary, apr_pool_t *pool, 
                                      int maxP, const char *authority, 
                                      const char **pdata, apr_size_t *plen)
{
    int nelts, N;
    unsigned char log2n, log2pmax;
    gset_encoder encoder;
    apr_uint64_t *hashes;
    apr_size_t hash_count, i;
    
    nelts = diary->entries->nelts;
    N = ceil_power_of_2(nelts);
    log2n = h2_log2(N);
    
    /* Now log2p is the max number of relevant bits, so that
     * log2p + log2n == mask_bits. We can use a lower log2p
     * and have a shorter set encoding...
     */
    log2pmax = h2_log2(ceil_power_of_2(maxP));
    
    memset(&encoder, 0, sizeof(encoder));
    encoder.diary = diary;
    encoder.log2p = H2MIN(diary->mask_bits - log2n, log2pmax);
    encoder.mask_bits = log2n + encoder.log2p;
    encoder.delta_bits = diary->mask_bits - encoder.mask_bits;
    encoder.fixed_bits = encoder.log2p;
    encoder.fixed_mask = 1;
    encoder.fixed_mask = (encoder.fixed_mask << encoder.fixed_bits) - 1;
    encoder.pool = pool;
    encoder.datalen = 512;
    encoder.data = apr_pcalloc(encoder.pool, encoder.datalen);
    
    encoder.data[0] = log2n;
    encoder.data[1] = encoder.log2p;
    encoder.offset = 1;
    encoder.bit = 8;
    encoder.last = 0;
    
    /* Intentional no APLOGNO */
    ap_log_perror(APLOG_MARK, GCSLOG_LEVEL, 0, pool,
                  "h2_push_diary_digest_get: %d entries, N=%d, log2n=%d, "
                  "mask_bits=%d, enc.mask_bits=%d, delta_bits=%d, enc.log2p=%d, authority=%s", 
                  (int)nelts, (int)N, (int)log2n, diary->mask_bits, 
                  (int)encoder.mask_bits, (int)encoder.delta_bits, 
                  (int)encoder.log2p, authority);
                  
    if (!authority || !diary->authority 
        || !strcmp("*", authority) || !strcmp(diary->authority, authority)) {
        hash_count = diary->entries->nelts;
        hashes = apr_pcalloc(encoder.pool, hash_count);
        for (i = 0; i < hash_count; ++i) {
            hashes[i] = ((&APR_ARRAY_IDX(diary->entries, i, h2_push_diary_entry))->hash 
                         >> encoder.delta_bits);
        }
        
        qsort(hashes, hash_count, sizeof(apr_uint64_t), cmp_puint64);
        for (i = 0; i < hash_count; ++i) {
            if (!i || (hashes[i] != hashes[i-1])) {
                gset_encode_next(&encoder, hashes[i]);
            }
        }
        /* Intentional no APLOGNO */
        ap_log_perror(APLOG_MARK, GCSLOG_LEVEL, 0, pool,
                      "h2_push_diary_digest_get: golomb compressed hashes, %d bytes",
                      (int)encoder.offset + 1);
    }
    *pdata = (const char *)encoder.data;
    *plen = encoder.offset + 1;
    
    return APR_SUCCESS;
}

