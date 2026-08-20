/*
 * ArgonOS - URLs, HTTP headers and FTP replies, without a socket in sight.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/netmsg.h>

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Small text helpers.  Written out rather than taken from <strings.h>:   */
/* the case-insensitive comparisons there are spelled differently on      */
/* every host this file is compiled for, and there are four of them.      */
/* ---------------------------------------------------------------------- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_space(char c) { return c == ' ' || c == '\t'; }

/* Case-insensitive compare of `s` against a whole word. */
static bool ieq(const char *s, const char *word)
{
    while (*word != '\0') {
        if (lower(*s) != lower(*word)) {
            return false;
        }
        s++;
        word++;
    }
    return *s == '\0';
}

/* Case-insensitive: does `s` begin with `prefix`? */
static bool ipre(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (lower(*s) != lower(*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

/* Case-insensitive substring, for header values that are lists. */
static bool ihas(const char *hay, const char *needle)
{
    for (; *hay != '\0'; hay++) {
        if (ipre(hay, needle)) {
            return true;
        }
    }
    return false;
}

/* Copies at most len-1 bytes and always terminates.  False if it did not fit:
 * the callers here treat truncation as a failure, not as a shorter answer. */
static bool copy_n(char *dst, size_t len, const char *src, size_t n)
{
    if (n + 1 > len) {
        return false;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

/* Unsigned decimal, the whole string or nothing. */
static bool parse_u64(const char *s, size_t n, uint64_t *out)
{
    if (n == 0) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) {
            return false;
        }
        /* An overflowing length is a lie about the body; refuse it. */
        if (v > (UINT64_MAX - (uint64_t)(s[i] - '0')) / 10u) {
            return false;
        }
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* ---------------------------------------------------------------------- */
/* Addresses                                                              */
/* ---------------------------------------------------------------------- */

bool ag_ipv4_parse(const char *text, uint32_t *addr_out)
{
    if (text == NULL || addr_out == NULL) {
        return false;
    }

    uint32_t addr = 0;
    const char *p = text;

    for (int group = 0; group < 4; group++) {
        if (!is_digit(*p)) {
            return false;
        }
        unsigned v = 0;
        int      digits = 0;
        while (is_digit(*p)) {
            v = v * 10u + (unsigned)(*p - '0');
            p++;
            if (++digits > 3 || v > 255u) {
                return false;
            }
        }
        addr = (addr << 8) | v;

        if (group < 3) {
            if (*p != '.') {
                return false;
            }
            p++;
        }
    }

    /* Trailing anything means this is a name, not an address. */
    if (*p != '\0') {
        return false;
    }
    *addr_out = addr;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

bool ag_mac_parse(const char *text, uint8_t out[6])
{
    if (text == NULL || out == NULL) {
        return false;
    }

    const char *p = text;
    for (int i = 0; i < 6; i++) {
        if (i > 0) {
            if (*p != ':' && *p != '-') {
                return false;
            }
            p++;
        }
        const int hi = hex_digit(p[0]);
        const int lo = (hi >= 0) ? hex_digit(p[1]) : -1;
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    /* Nothing after the sixth pair: "aa:bb:cc:dd:ee:ff:00" is not an address
     * with a spare byte, it is a typing mistake. */
    return *p == '\0';
}

int ag_mac_str(const uint8_t mac[6], char *buf, size_t len)
{
    static const char k_hex[] = "0123456789abcdef";

    if (buf == NULL || mac == NULL || len < 18) {
        return 0;
    }
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        if (i > 0) {
            buf[pos++] = ':';
        }
        buf[pos++] = k_hex[(mac[i] >> 4) & 0x0fu];
        buf[pos++] = k_hex[mac[i] & 0x0fu];
    }
    buf[pos] = '\0';
    return pos;
}

int ag_ipv4_str(uint32_t addr, char *buf, size_t len)
{
    if (buf == NULL || len < 8) {
        return 0;
    }

    int pos = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const unsigned byte = (addr >> shift) & 0xffu;
        if (shift != 24) {
            if ((size_t)pos + 1 >= len) {
                break;
            }
            buf[pos++] = '.';
        }
        char digits[3];
        int  n = 0;
        unsigned v = byte;
        do {
            digits[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        } while (v != 0);
        while (n > 0 && (size_t)pos + 1 < len) {
            buf[pos++] = digits[--n];
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* ---------------------------------------------------------------------- */
/* URLs                                                                   */
/* ---------------------------------------------------------------------- */

ag_err_t ag_url_parse(const char *text, ag_url_t *out)
{
    if (text == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));

    const char *sep = strstr(text, "://");
    if (sep == NULL) {
        return -AG_EINVAL;
    }
    if (!copy_n(out->scheme, sizeof(out->scheme), text, (size_t)(sep - text))) {
        return -AG_ENOTSUP;
    }
    for (char *s = out->scheme; *s != '\0'; s++) {
        *s = lower(*s);
    }

    if (strcmp(out->scheme, "http") == 0) {
        out->port = 80;
    } else if (strcmp(out->scheme, "ftp") == 0) {
        out->port = 21;
    } else {
        /*
         * https lands here, and it is the reason this returns a distinct code:
         * there is no TLS in this system, and quietly fetching an https URL
         * over port 80 would send a password in clear for a caller who took
         * care to ask for encryption.
         */
        return -AG_ENOTSUP;
    }

    const char *auth = sep + 3;
    const char *path = auth;
    while (*path != '\0' && *path != '/') {
        path++;
    }

    /* userinfo ends at the last '@' before the path: a password may contain
     * one, a hostname may not. */
    const char *at = NULL;
    for (const char *p = auth; p < path; p++) {
        if (*p == '@') {
            at = p;
        }
    }

    const char *hostpart = auth;
    if (at != NULL) {
        const char *colon = NULL;
        for (const char *p = auth; p < at; p++) {
            if (*p == ':') {
                colon = p;
                break;
            }
        }
        const char *user_end = (colon != NULL) ? colon : at;
        if (!copy_n(out->user, sizeof(out->user), auth,
                    (size_t)(user_end - auth))) {
            return -AG_ERANGE;
        }
        if (colon != NULL &&
            !copy_n(out->pass, sizeof(out->pass), colon + 1,
                    (size_t)(at - colon - 1))) {
            return -AG_ERANGE;
        }
        hostpart = at + 1;
    }

    /* No IPv6 anywhere below this: the port layer is IPv4 and says so. */
    if (*hostpart == '[') {
        return -AG_ENOTSUP;
    }

    const char *colon = NULL;
    for (const char *p = hostpart; p < path; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }
    const char *host_end = (colon != NULL) ? colon : path;
    if (host_end == hostpart) {
        return -AG_EINVAL;
    }
    if (!copy_n(out->host, sizeof(out->host), hostpart,
                (size_t)(host_end - hostpart))) {
        return -AG_ERANGE;
    }

    if (colon != NULL) {
        uint64_t port = 0;
        if (!parse_u64(colon + 1, (size_t)(path - colon - 1), &port) ||
            port == 0 || port > 65535u) {
            return -AG_EINVAL;
        }
        out->port = (uint16_t)port;
    }

    if (*path == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else if (!copy_n(out->path, sizeof(out->path), path, strlen(path))) {
        return -AG_ERANGE;
    }
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* HTTP                                                                   */
/* ---------------------------------------------------------------------- */

size_t ag_http_header_end(const char *buf, size_t len)
{
    if (buf == NULL) {
        return 0;
    }
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            return i + 2;
        }
        if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

/*
 * One line of a header block: `line` and `line_len` come back pointing at it
 * without its terminator, and the return value is where the next line starts.
 * Returns NULL at the end of the block.
 */
static const char *next_line(const char *p, const char *end,
                             const char **line, size_t *line_len)
{
    if (p >= end) {
        return NULL;
    }
    const char *nl = p;
    while (nl < end && *nl != '\n') {
        nl++;
    }
    const char *stop = nl;
    if (stop > p && stop[-1] == '\r') {
        stop--;
    }
    *line = p;
    *line_len = (size_t)(stop - p);
    return (nl < end) ? nl + 1 : end;
}

/* Splits "Name: value" into a lowercase-comparable name and a trimmed value.
 * The value is copied out because the header block is not NUL-terminated. */
static bool split_header(const char *line, size_t len, const char **name,
                         size_t *name_len, char *value, size_t value_size)
{
    size_t colon = 0;
    while (colon < len && line[colon] != ':') {
        colon++;
    }
    if (colon == len) {
        return false;
    }
    *name = line;
    *name_len = colon;

    size_t start = colon + 1;
    while (start < len && is_space(line[start])) {
        start++;
    }
    size_t stop = len;
    while (stop > start && is_space(line[stop - 1])) {
        stop--;
    }

    const size_t n = stop - start;
    const size_t fit = (n + 1 > value_size) ? value_size - 1 : n;
    memcpy(value, line + start, fit);
    value[fit] = '\0';
    return true;
}

/* Compares a header name given as a length-delimited slice. */
static bool name_is(const char *name, size_t len, const char *want)
{
    if (strlen(want) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (lower(name[i]) != lower(want[i])) {
            return false;
        }
    }
    return true;
}

ag_err_t ag_http_parse_response(const char *hdr, size_t len,
                                ag_http_resp_t *out)
{
    if (hdr == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));

    const char *end = hdr + len;
    const char *line = NULL;
    size_t      line_len = 0;
    const char *p = next_line(hdr, end, &line, &line_len);
    if (p == NULL) {
        return -AG_EFORMAT;
    }

    if (line_len < 12 || !ipre(line, "HTTP/")) {
        return -AG_EFORMAT;
    }
    /* "HTTP/1.1 200 ..." - the status is where the first space is, not at a
     * fixed offset: HTTP/1.10 is legal and would shift it. */
    size_t i = 5;
    while (i < line_len && line[i] != ' ') {
        i++;
    }
    while (i < line_len && line[i] == ' ') {
        i++;
    }
    if (i + 3 > line_len || !is_digit(line[i]) || !is_digit(line[i + 1]) ||
        !is_digit(line[i + 2])) {
        return -AG_EFORMAT;
    }
    out->status = (line[i] - '0') * 100 + (line[i + 1] - '0') * 10 +
                  (line[i + 2] - '0');

    /* HTTP/1.0 has no persistent connections unless it asks for one. */
    out->close_after = ipre(line, "HTTP/1.0");

    char value[288];
    while ((p = next_line(p, end, &line, &line_len)) != NULL) {
        if (line_len == 0) {
            break; /* the blank line: end of the block */
        }
        const char *name = NULL;
        size_t      name_len = 0;
        if (!split_header(line, line_len, &name, &name_len, value,
                          sizeof(value))) {
            continue; /* not a header; a continuation line or noise */
        }

        if (name_is(name, name_len, "content-length")) {
            uint64_t v = 0;
            if (parse_u64(value, strlen(value), &v)) {
                out->length = v;
                out->have_length = true;
            }
        } else if (name_is(name, name_len, "transfer-encoding")) {
            if (ihas(value, "chunked")) {
                out->chunked = true;
            }
        } else if (name_is(name, name_len, "connection")) {
            if (ihas(value, "close")) {
                out->close_after = true;
            } else if (ihas(value, "keep-alive")) {
                out->close_after = false;
            }
        } else if (name_is(name, name_len, "location")) {
            /*
             * A target that does not fit is left empty rather than shortened.
             * Half a URL is a request for something else entirely, and the
             * caller can say "redirect too long" only if it can tell.
             */
            if (strlen(value) < sizeof(out->location)) {
                memcpy(out->location, value, strlen(value) + 1);
            }
        } else if (name_is(name, name_len, "content-type")) {
            const size_t n = strlen(value);
            const size_t fit =
                (n + 1 > sizeof(out->type)) ? sizeof(out->type) - 1 : n;
            memcpy(out->type, value, fit);
            out->type[fit] = '\0';
        }
    }

    /* A body that is chunked has no length, whatever a Content-Length said. */
    if (out->chunked) {
        out->have_length = false;
        out->length = 0;
    }
    return AG_OK;
}

ag_err_t ag_http_parse_request(const char *hdr, size_t len,
                               ag_http_req_t *out)
{
    if (hdr == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));

    const char *end = hdr + len;
    const char *line = NULL;
    size_t      line_len = 0;
    const char *p = next_line(hdr, end, &line, &line_len);
    if (p == NULL || line_len == 0) {
        return -AG_EFORMAT;
    }

    size_t i = 0;
    while (i < line_len && line[i] != ' ') {
        i++;
    }
    if (i == 0 || i >= sizeof(out->method)) {
        return -AG_EFORMAT;
    }
    for (size_t k = 0; k < i; k++) {
        out->method[k] = upper(line[k]);
    }
    out->method[i] = '\0';

    while (i < line_len && line[i] == ' ') {
        i++;
    }
    size_t target_start = i;
    while (i < line_len && line[i] != ' ') {
        i++;
    }
    size_t target_len = i - target_start;
    if (target_len == 0) {
        return -AG_EFORMAT;
    }

    /*
     * The query is cut before decoding, not after.  A '?' that arrives as %3F
     * is part of a filename; one that arrives as itself is not, and only the
     * undecoded form can tell them apart.
     */
    for (size_t k = 0; k < target_len; k++) {
        if (line[target_start + k] == '?') {
            target_len = k;
            break;
        }
    }
    if (!copy_n(out->target, sizeof(out->target), line + target_start,
                target_len)) {
        return -AG_ERANGE;
    }
    (void)ag_pct_decode(out->target);

    /* The version is the rest of the line, and it sets the default. */
    while (i < line_len && line[i] == ' ') {
        i++;
    }
    out->keep_alive = (i < line_len) && ipre(line + i, "HTTP/1.1");

    char value[288];
    while ((p = next_line(p, end, &line, &line_len)) != NULL) {
        if (line_len == 0) {
            break;
        }
        const char *name = NULL;
        size_t      name_len = 0;
        if (!split_header(line, line_len, &name, &name_len, value,
                          sizeof(value))) {
            continue;
        }
        if (name_is(name, name_len, "connection")) {
            if (ihas(value, "close")) {
                out->keep_alive = false;
            } else if (ihas(value, "keep-alive")) {
                out->keep_alive = true;
            }
        } else if (name_is(name, name_len, "content-type")) {
            (void)copy_n(out->content_type, sizeof(out->content_type), value,
                         strlen(value));
        } else if (name_is(name, name_len, "content-length")) {
            uint64_t n = 0;
            if (parse_u64(value, strlen(value), &n)) {
                out->content_length = n;
                out->have_length = true;
            }
        }
    }
    return AG_OK;
}

/* ---------------------------------------------------------------------- */

/* The value of a parameter in a header: `; name=value` or `; name="value"`. */
static bool param_value(const char *text, const char *name, char *out,
                        size_t len)
{
    const size_t name_len = strlen(name);

    for (const char *p = text; *p != '\0'; p++) {
        if (!ipre(p, name)) {
            continue;
        }
        /* The name has to start a parameter, not end another one's value. */
        if (p != text) {
            const char before = p[-1];
            if (before != ' ' && before != ';' && before != '\t') {
                continue;
            }
        }
        const char *v = p + name_len;
        while (*v == ' ') {
            v++;
        }
        if (*v != '=') {
            continue;
        }
        v++;
        while (*v == ' ') {
            v++;
        }

        const char stop = (*v == '"') ? '"' : ';';
        if (*v == '"') {
            v++;
        }
        size_t n = 0;
        while (v[n] != '\0' && v[n] != stop && v[n] != '\r' && v[n] != '\n') {
            n++;
        }
        /* An unquoted value also ends at whitespace; a quoted one does not,
         * because a filename with a space in it is an ordinary filename. */
        if (stop == ';') {
            size_t m = 0;
            while (m < n && v[m] != ' ' && v[m] != '\t') {
                m++;
            }
            n = m;
        }
        return copy_n(out, len, v, n);
    }
    return false;
}

bool ag_http_boundary(const char *content_type, char *out, size_t len)
{
    if (content_type == NULL || out == NULL || len == 0) {
        return false;
    }
    out[0] = '\0';
    if (!ipre(content_type, "multipart/form-data")) {
        return false;
    }
    if (!param_value(content_type, "boundary", out, len)) {
        return false;
    }
    return out[0] != '\0';
}

bool ag_http_part_filename(const char *headers, size_t len, char *out,
                           size_t out_len)
{
    if (headers == NULL || out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    const char *end = headers + len;
    const char *line = NULL;
    size_t      line_len = 0;
    const char *p = headers;
    char        value[288];
    bool        found = false;

    while ((p = next_line(p, end, &line, &line_len)) != NULL) {
        if (line_len == 0) {
            break;
        }
        const char *name = NULL;
        size_t      name_len = 0;
        if (!split_header(line, line_len, &name, &name_len, value,
                          sizeof(value))) {
            continue;
        }
        if (name_is(name, name_len, "content-disposition") &&
            param_value(value, "filename", out, out_len)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }

    /* Whatever the client called it, only the last component is a name here. */
    const char *base = out;
    for (const char *q = out; *q != '\0'; q++) {
        if (*q == '/' || *q == '\\' || *q == ':') {
            base = q + 1;
        }
    }
    if (base != out) {
        memmove(out, base, strlen(base) + 1);
    }

    if (out[0] == '\0' || strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        out[0] = '\0';
        return false;
    }
    for (const char *q = out; *q != '\0'; q++) {
        if ((unsigned char)*q < 0x20u || *q == 0x7f) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

bool ag_http_target_safe(const char *target)
{
    if (target == NULL || target[0] != '/') {
        return false;
    }

    for (const char *p = target; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        /*
         * Control characters, a backslash and a colon are all refused rather
         * than escaped: on this system a backslash is a path separator and a
         * colon introduces a drive, so "/..\\" and "/c:/" are both ways of
         * asking for a file outside the served tree.
         */
        if (c < 0x20u || c == 0x7fu || c == '\\' || c == ':') {
            return false;
        }
    }

    /* No segment may be "..", however it was spelled before decoding. */
    const char *seg = target + 1;
    while (*seg != '\0') {
        const char *slash = strchr(seg, '/');
        const size_t n = (slash != NULL) ? (size_t)(slash - seg) : strlen(seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            return false;
        }
        if (slash == NULL) {
            break;
        }
        seg = slash + 1;
    }
    return true;
}

ag_err_t ag_http_chunk_size(const char *line, uint64_t *out)
{
    if (line == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    while (is_space(*line)) {
        line++;
    }

    uint64_t v = 0;
    int      digits = 0;
    for (; *line != '\0'; line++) {
        const int d = hex_val(*line);
        if (d < 0) {
            break;
        }
        if (v > (UINT64_MAX >> 4)) {
            return -AG_ERANGE;
        }
        v = (v << 4) | (uint64_t)d;
        digits++;
    }
    if (digits == 0) {
        return -AG_EINVAL;
    }

    /* Anything after the size must be a chunk extension or the terminator. */
    while (is_space(*line)) {
        line++;
    }
    if (*line != '\0' && *line != ';' && *line != '\r' && *line != '\n') {
        return -AG_EINVAL;
    }
    *out = v;
    return AG_OK;
}

size_t ag_pct_decode(char *s)
{
    if (s == NULL) {
        return 0;
    }
    char *read = s;
    char *write = s;

    while (*read != '\0') {
        if (*read == '%') {
            const int hi = hex_val(read[1]);
            const int lo = (hi >= 0) ? hex_val(read[2]) : -1;
            if (lo >= 0) {
                *write++ = (char)((hi << 4) | lo);
                read += 3;
                continue;
            }
            /* A '%' that is not an escape stays a '%'.  Eating the two bytes
             * after it would turn a filename into a different filename. */
        }
        *write++ = *read++;
    }
    *write = '\0';
    return (size_t)(write - s);
}

size_t ag_pct_encode(const char *in, char *out, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";

    if (in == NULL || out == NULL || len == 0) {
        return 0;
    }

    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != 0; p++) {
        const unsigned char c = *p;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                           c == '.' || c == '~';
        if (plain) {
            if (w + 1 >= len) {
                return 0;
            }
            out[w++] = (char)c;
        } else {
            if (w + 3 >= len) {
                return 0;
            }
            out[w++] = '%';
            out[w++] = hex[(c >> 4) & 0x0fu];
            out[w++] = hex[c & 0x0fu];
        }
    }
    out[w] = 0;
    return w;
}

const char *ag_http_mime(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } table[] = {
        {"htm", "text/html"},        {"html", "text/html"},
        {"txt", "text/plain"},       {"md", "text/plain"},
        {"c", "text/plain"},         {"h", "text/plain"},
        {"cfg", "text/plain"},       {"bat", "text/plain"},
        {"log", "text/plain"},       {"json", "application/json"},
        {"css", "text/css"},         {"js", "text/javascript"},
        {"png", "image/png"},        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},      {"gif", "image/gif"},
        {"bmp", "image/bmp"},        {"ico", "image/x-icon"},
        {"ppm", "image/x-portable-pixmap"},
        {"wav", "audio/wav"},        {"mp3", "audio/mpeg"},
        {"mid", "audio/midi"},       {"zip", "application/zip"},
        {"pdf", "application/pdf"},
    };

    if (path == NULL) {
        return "application/octet-stream";
    }
    const char *dot = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        } else if (*p == '/' || *p == '\\') {
            dot = NULL;
        }
    }
    if (dot != NULL) {
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            if (ieq(dot + 1, table[i].ext)) {
                return table[i].type;
            }
        }
    }
    /* Everything else is bytes.  A browser asked to guess at an .AXE image
     * would try to display it. */
    return "application/octet-stream";
}

const char *ag_http_status_text(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 414:
        return "URI Too Long";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return "";
    }
}

/* ---------------------------------------------------------------------- */
/* FTP                                                                    */
/* ---------------------------------------------------------------------- */

int ag_ftp_reply_code(const char *line, bool *final_out)
{
    if (final_out != NULL) {
        *final_out = false;
    }
    if (line == NULL || !is_digit(line[0]) || !is_digit(line[1]) ||
        !is_digit(line[2])) {
        return -1;
    }
    const char c = line[3];
    if (c != '\0' && c != ' ' && c != '-' && c != '\r' && c != '\n') {
        return -1;
    }
    if (final_out != NULL) {
        *final_out = (c != '-');
    }
    return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

ag_err_t ag_ftp_pasv_parse(const char *line, uint32_t *addr, uint16_t *port)
{
    if (line == NULL || addr == NULL || port == NULL) {
        return -AG_EINVAL;
    }

    /*
     * Six numbers separated by commas, found by looking for six numbers
     * separated by commas.  The sentence around them is different on every
     * server - "Entering Passive Mode", "=", a translated phrase - and none of
     * it is part of the protocol.
     */
    for (const char *start = line; *start != '\0'; start++) {
        if (!is_digit(*start) ||
            (start != line && (is_digit(start[-1]) || start[-1] == '.'))) {
            continue;
        }

        unsigned    n[6];
        const char *p = start;
        int         got = 0;
        while (got < 6) {
            if (!is_digit(*p)) {
                break;
            }
            unsigned v = 0;
            int      digits = 0;
            while (is_digit(*p)) {
                v = v * 10u + (unsigned)(*p - '0');
                p++;
                if (++digits > 3 || v > 255u) {
                    break;
                }
            }
            if (v > 255u || digits > 3) {
                break;
            }
            n[got++] = v;
            if (got < 6) {
                if (*p != ',') {
                    break;
                }
                p++;
            }
        }

        if (got == 6 && !is_digit(*p)) {
            *addr = (n[0] << 24) | (n[1] << 16) | (n[2] << 8) | n[3];
            *port = (uint16_t)((n[4] << 8) | n[5]);
            return AG_OK;
        }
    }
    return -AG_EFORMAT;
}

ag_err_t ag_ftp_size_parse(const char *line, uint64_t *out)
{
    if (line == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    bool      final = false;
    const int code = ag_ftp_reply_code(line, &final);
    if (code != 213 || !final) {
        return -AG_EINVAL;
    }

    const char *p = line + 3;
    while (is_space(*p)) {
        p++;
    }
    size_t n = 0;
    while (is_digit(p[n])) {
        n++;
    }
    if (n == 0) {
        return -AG_EINVAL;
    }
    /* Only digits, then the end of the line: a sentence means no size. */
    for (const char *tail = p + n; *tail != '\0'; tail++) {
        if (*tail != '\r' && *tail != '\n' && !is_space(*tail)) {
            return -AG_EINVAL;
        }
    }
    return parse_u64(p, n, out) ? AG_OK : -AG_ERANGE;
}
