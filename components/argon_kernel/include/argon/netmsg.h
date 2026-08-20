/*
 * ArgonOS - the half of the network protocols that is text.
 *
 * URLs, HTTP headers, FTP replies.  Not one socket between them: everything
 * here takes a buffer and gives back a struct, which is why it can be tested
 * on a machine with no network at all - and why the client, the server and the
 * file transfer do not each grow their own parser.
 *
 * That split is not tidiness.  Every protocol defect worth having is in this
 * file's business: a status line that is not 200, a length that is a chunk
 * count, a path that climbs out of the document root, a passive-mode reply
 * whose six numbers are an address.  Those are the things a test can pin down
 * without a wire, and the things that are unpinnable once they are tangled up
 * with recv().
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NETMSG_H
#define ARGON_NETMSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bounded on purpose, and small on purpose.  This is a chip with 320 KB of
 * internal memory in total, so a URL that does not fit is refused rather than
 * quietly truncated - a truncated path is a request for the wrong file, which
 * is worse than an error message.
 */
#define AG_URL_HOST_MAX 64
#define AG_URL_USER_MAX 32
#define AG_URL_PASS_MAX 32
#define AG_URL_PATH_MAX 192

typedef struct {
    char     scheme[8]; /* lowercased: "http" or "ftp"                     */
    char     host[AG_URL_HOST_MAX + 1];
    char     user[AG_URL_USER_MAX + 1]; /* "" when the URL carried none    */
    char     pass[AG_URL_PASS_MAX + 1];
    uint16_t port; /* the scheme's default when the URL omitted it         */
    char     path[AG_URL_PATH_MAX + 1]; /* always starts with '/'          */
} ag_url_t;

/* Host-order IPv4 from a dotted quad.  False for anything else, including a
 * name that merely starts with digits. */
bool ag_ipv4_parse(const char *text, uint32_t *addr_out);

/* Host-order IPv4 into `buf` as a dotted quad.  Returns the length written. */
int ag_ipv4_str(uint32_t addr, char *buf, size_t len);

/*
 * Six bytes from "aa:bb:cc:dd:ee:ff", either case, and dashes accepted because
 * that is how Windows writes them.  False for anything else - a short one
 * included, since half a hardware address is not a hardware address.
 */
bool ag_mac_parse(const char *text, uint8_t out[6]);

/* Six bytes into `buf` as lower-case colon-separated pairs.  Wants 18 bytes;
 * returns the length written, or 0 when it does not fit. */
int ag_mac_str(const uint8_t mac[6], char *buf, size_t len);

/*
 * "http://user:pass@host:port/path" and the same with ftp.  A scheme is
 * required: guessing http for a bare host looks helpful right up to the point
 * where a typed "ftp.example.com/x" silently talks to the wrong server.
 *
 * -AG_EINVAL for anything malformed, -AG_ENOTSUP for a scheme this system
 * does not speak (https above all - there is no TLS here, and a URL that
 * would be sent in clear must be refused rather than downgraded), -AG_ERANGE
 * when a component does not fit.
 */
ag_err_t ag_url_parse(const char *text, ag_url_t *out);

/* ---------------------------------------------------------------------- */
/* HTTP                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * Where a header block ends inside `buf`: the offset just past the blank line,
 * or 0 while the block is still incomplete.  Accepts a bare LF as well as
 * CRLF, because something on the other end always does.
 */
size_t ag_http_header_end(const char *buf, size_t len);

typedef struct {
    int      status;      /* 200, 404, ...                                 */
    bool     have_length; /* Content-Length was present and understood     */
    uint64_t length;
    bool     chunked;     /* Transfer-Encoding: chunked                    */
    bool     close_after; /* the server said it will close                 */
    char     location[256]; /* 3xx target, "" otherwise                    */
    char     type[48];      /* Content-Type, "" when absent                */
} ag_http_resp_t;

/*
 * The response header block, from the status line to the blank line.  Headers
 * this system has no use for are skipped rather than rejected: a client that
 * fails on an unknown header is a client that fails on a new server.
 */
ag_err_t ag_http_parse_response(const char *hdr, size_t len,
                                ag_http_resp_t *out);

/* Long enough for "multipart/form-data; boundary=..." as browsers send it. */
#define AG_HTTP_CTYPE_MAX 128
/* RFC 2046 caps a boundary at 70 characters. */
#define AG_HTTP_BOUNDARY_MAX 70

typedef struct {
    char method[8];                     /* upper case, as it arrived       */
    char target[AG_URL_PATH_MAX + 1];   /* percent-decoded, query removed  */
    bool keep_alive;

    /* What a body, if any, is and how long.  Empty and zero for a GET. */
    char     content_type[AG_HTTP_CTYPE_MAX + 1];
    uint64_t content_length;
    bool     have_length;
} ag_http_req_t;

/*
 * The request line and the headers that decide what happens next.  The target
 * is decoded here and checked by ag_http_target_safe below - decoding first
 * and checking after is the whole of the directory-traversal bug, so the two
 * live next to each other.
 */
ag_err_t ag_http_parse_request(const char *hdr, size_t len,
                               ag_http_req_t *out);

/*
 * The boundary out of "multipart/form-data; boundary=----WebKitFormBoundary".
 *
 * False when the type is not multipart or names no boundary - both of which a
 * server must refuse rather than guess at, because without the boundary there
 * is no way to know where the file ends.
 */
bool ag_http_boundary(const char *content_type, char *out, size_t len);

/*
 * The filename out of one part's headers:
 *
 *   Content-Disposition: form-data; name="file"; filename="holiday.jpg"
 *
 * Answers the *base* name.  Some browsers - and every phone that has ever sent
 * a photo from a gallery - put a whole path in there, and a server that joined
 * that to its root would write wherever the client said.  Parts with no
 * filename (an ordinary form field) answer false, as do names that cannot be
 * written: empty, "." and "..", or anything with a separator left in it.
 */
bool ag_http_part_filename(const char *headers, size_t len, char *out,
                           size_t out_len);

/*
 * True when a decoded target can be joined to a document root and stay inside
 * it.  Rejects "..", backslashes, drive letters, embedded NULs and anything
 * that does not begin with '/'.  Checked after decoding, because "%2e%2e" is
 * ".." and only the decoded form can be judged.
 */
bool ag_http_target_safe(const char *target);

/* A chunked-encoding size line ("1a2b" or "1a2b;ext=x").  -AG_EINVAL if it is
 * not hexadecimal. */
ag_err_t ag_http_chunk_size(const char *line, uint64_t *out);

/* Percent-decoding in place.  Returns the new length; a stray '%' is left as
 * itself rather than eating the bytes after it. */
size_t ag_pct_decode(char *s);

/*
 * The other direction, for a link in a generated page: everything that is not
 * unreserved becomes %XX.  Returns the length written, or 0 when it does not
 * fit - and 0 means "do not link to this", never "link to the short version".
 */
size_t ag_pct_encode(const char *in, char *out, size_t len);

/* Content-Type by file extension.  Never NULL: unknown is a byte stream. */
const char *ag_http_mime(const char *path);

/* The reason phrase for the few statuses this system sends. */
const char *ag_http_status_text(int status);

/* ---------------------------------------------------------------------- */
/* FTP                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * The three-digit code at the front of a reply line, or -1 if there is none.
 * `final_out` is false for a continuation line ("220-text"), which is how a
 * multi-line greeting is told from the end of one - get that wrong and the
 * next command is answered by the tail of the last reply.
 */
int ag_ftp_reply_code(const char *line, bool *final_out);

/*
 * The address in a 227 reply: "Entering Passive Mode (10,0,2,2,195,149)".
 * Host-order address and port out.  The numbers are found by scanning for the
 * parenthesised group rather than by matching the sentence, because the
 * sentence is different on every server.
 */
ag_err_t ag_ftp_pasv_parse(const char *line, uint32_t *addr, uint16_t *port);

/*
 * The size in a 213 reply, and nothing else: a server that answers SIZE with a
 * sentence gets -AG_EINVAL and the transfer runs without a total.
 */
ag_err_t ag_ftp_size_parse(const char *line, uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NETMSG_H */
