/*
 * ArgonOS - URL, HTTP and FTP text tests.
 *
 * These run on a machine with no network, which is the point: every defect
 * that matters in a client or a server is in the text, and text can be pinned
 * down.  The cases that are not obvious are the ones that were wrong once -
 * a status line at a fixed offset, a query cut after decoding rather than
 * before, a passive-mode reply matched by its sentence.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/netmsg.h>

#include <string.h>

#include "test.h"

static void test_ipv4(void)
{
    uint32_t a = 0;

    AG_CHECK(ag_ipv4_parse("10.0.2.2", &a));
    AG_CHECK_INT(a, 0x0a000202u);
    AG_CHECK(ag_ipv4_parse("0.0.0.0", &a));
    AG_CHECK_INT(a, 0);
    AG_CHECK(ag_ipv4_parse("255.255.255.255", &a));
    AG_CHECK_INT(a, 0xffffffffu);

    /* A name that happens to start with digits is not an address. */
    AG_CHECK(!ag_ipv4_parse("1.2.3.4.example.com", &a));
    AG_CHECK(!ag_ipv4_parse("10.0.2", &a));
    AG_CHECK(!ag_ipv4_parse("10.0.2.256", &a));
    AG_CHECK(!ag_ipv4_parse("10.0.2.2.", &a));
    AG_CHECK(!ag_ipv4_parse("10.0.2.02x", &a));
    AG_CHECK(!ag_ipv4_parse("example.com", &a));
    AG_CHECK(!ag_ipv4_parse("", &a));

    char buf[16];
    AG_CHECK_INT(ag_ipv4_str(0x0a000202u, buf, sizeof(buf)), 8);
    AG_CHECK_STR(buf, "10.0.2.2");
    AG_CHECK_INT(ag_ipv4_str(0xffffffffu, buf, sizeof(buf)), 15);
    AG_CHECK_STR(buf, "255.255.255.255");
    AG_CHECK_INT(ag_ipv4_str(0, buf, sizeof(buf)), 7);
    AG_CHECK_STR(buf, "0.0.0.0");
}

static void test_url(void)
{
    ag_url_t u;

    AG_CHECK_INT(ag_url_parse("http://example.com/a/b.txt", &u), AG_OK);
    AG_CHECK_STR(u.scheme, "http");
    AG_CHECK_STR(u.host, "example.com");
    AG_CHECK_INT(u.port, 80);
    AG_CHECK_STR(u.path, "/a/b.txt");
    AG_CHECK_STR(u.user, "");

    /* No path at all still asks for the root. */
    AG_CHECK_INT(ag_url_parse("http://10.0.2.2", &u), AG_OK);
    AG_CHECK_STR(u.host, "10.0.2.2");
    AG_CHECK_STR(u.path, "/");

    AG_CHECK_INT(ag_url_parse("HTTP://10.0.2.2:8000/f.bin", &u), AG_OK);
    AG_CHECK_STR(u.scheme, "http");
    AG_CHECK_INT(u.port, 8000);
    AG_CHECK_STR(u.path, "/f.bin");

    /* The query travels with the path: it is the server's business. */
    AG_CHECK_INT(ag_url_parse("http://h/cgi?a=1&b=2", &u), AG_OK);
    AG_CHECK_STR(u.path, "/cgi?a=1&b=2");

    AG_CHECK_INT(ag_url_parse("ftp://ftp.example.com/pub/", &u), AG_OK);
    AG_CHECK_STR(u.scheme, "ftp");
    AG_CHECK_INT(u.port, 21);
    AG_CHECK_STR(u.path, "/pub/");

    AG_CHECK_INT(ag_url_parse("ftp://bob:s3cret@host:2121/x", &u), AG_OK);
    AG_CHECK_STR(u.user, "bob");
    AG_CHECK_STR(u.pass, "s3cret");
    AG_CHECK_STR(u.host, "host");
    AG_CHECK_INT(u.port, 2121);

    /* A password may contain '@'; a hostname may not, so the last one wins. */
    AG_CHECK_INT(ag_url_parse("ftp://bob:a@b@host/x", &u), AG_OK);
    AG_CHECK_STR(u.pass, "a@b");
    AG_CHECK_STR(u.host, "host");

    AG_CHECK_INT(ag_url_parse("ftp://anonymous@host/x", &u), AG_OK);
    AG_CHECK_STR(u.user, "anonymous");
    AG_CHECK_STR(u.pass, "");

    /* https is refused loudly rather than downgraded to port 80. */
    AG_CHECK_INT(ag_url_parse("https://example.com/", &u), -AG_ENOTSUP);
    AG_CHECK_INT(ag_url_parse("gopher://example.com/", &u), -AG_ENOTSUP);
    AG_CHECK_INT(ag_url_parse("http://[::1]/", &u), -AG_ENOTSUP);

    AG_CHECK_INT(ag_url_parse("example.com/x", &u), -AG_EINVAL);
    AG_CHECK_INT(ag_url_parse("http:///x", &u), -AG_EINVAL);
    AG_CHECK_INT(ag_url_parse("http://h:0/x", &u), -AG_EINVAL);
    AG_CHECK_INT(ag_url_parse("http://h:99999/x", &u), -AG_EINVAL);
    AG_CHECK_INT(ag_url_parse("http://h:80x/x", &u), -AG_EINVAL);

    /* Too long is an error, never a shorter request for another file. */
    char       long_url[AG_URL_PATH_MAX + 64];
    const char prefix[] = "http://h/";
    memcpy(long_url, prefix, sizeof(prefix) - 1);
    memset(long_url + sizeof(prefix) - 1, 'a', sizeof(long_url) - sizeof(prefix));
    long_url[sizeof(long_url) - 1] = '\0';
    AG_CHECK_INT(ag_url_parse(long_url, &u), -AG_ERANGE);
}

static void test_http_header_end(void)
{
    const char crlf[] = "HTTP/1.1 200 OK\r\nA: b\r\n\r\nbody";
    AG_CHECK_INT(ag_http_header_end(crlf, strlen(crlf)), 25);

    const char lf[] = "HTTP/1.1 200 OK\nA: b\n\nbody";
    AG_CHECK_INT(ag_http_header_end(lf, strlen(lf)), 22);

    const char partial[] = "HTTP/1.1 200 OK\r\nA: b\r\n";
    AG_CHECK_INT(ag_http_header_end(partial, strlen(partial)), 0);

    /* A blank line that has not finished arriving is not a blank line. */
    const char split[] = "HTTP/1.1 200 OK\r\n\r";
    AG_CHECK_INT(ag_http_header_end(split, strlen(split)), 0);
}

static void test_http_response(void)
{
    ag_http_resp_t r;

    const char ok[] = "HTTP/1.1 200 OK\r\n"
                      "Server: nginx\r\n"
                      "Content-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: 1234\r\n"
                      "\r\n";
    AG_CHECK_INT(ag_http_parse_response(ok, strlen(ok), &r), AG_OK);
    AG_CHECK_INT(r.status, 200);
    AG_CHECK(r.have_length);
    AG_CHECK_INT((long)r.length, 1234);
    AG_CHECK(!r.chunked);
    AG_CHECK(!r.close_after);
    AG_CHECK_STR(r.type, "text/html; charset=utf-8");

    /* Header names are case-insensitive and the value may be padded. */
    const char odd[] = "HTTP/1.1 200 OK\r\n"
                       "CONTENT-LENGTH:   7   \r\n"
                       "coNNection: Close\r\n"
                       "\r\n";
    AG_CHECK_INT(ag_http_parse_response(odd, strlen(odd), &r), AG_OK);
    AG_CHECK_INT((long)r.length, 7);
    AG_CHECK(r.close_after);

    /* Chunked wins: a length alongside it would be the wrong length. */
    const char chunked[] = "HTTP/1.1 200 OK\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "Content-Length: 99\r\n"
                           "\r\n";
    AG_CHECK_INT(ag_http_parse_response(chunked, strlen(chunked), &r), AG_OK);
    AG_CHECK(r.chunked);
    AG_CHECK(!r.have_length);
    AG_CHECK_INT((long)r.length, 0);

    /* HTTP/1.0 closes unless it says otherwise. */
    const char ten[] = "HTTP/1.0 200 OK\r\nContent-Length: 1\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_response(ten, strlen(ten), &r), AG_OK);
    AG_CHECK(r.close_after);
    const char ten_ka[] =
        "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 1\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_response(ten_ka, strlen(ten_ka), &r), AG_OK);
    AG_CHECK(!r.close_after);

    const char moved[] = "HTTP/1.1 301 Moved Permanently\r\n"
                         "Location: http://other.example/x.bin\r\n"
                         "Content-Length: 0\r\n"
                         "\r\n";
    AG_CHECK_INT(ag_http_parse_response(moved, strlen(moved), &r), AG_OK);
    AG_CHECK_INT(r.status, 301);
    AG_CHECK_STR(r.location, "http://other.example/x.bin");

    /* A status line is not at a fixed offset: HTTP/1.10 would shift it. */
    const char weird[] = "HTTP/1.10 404 Not Found\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_response(weird, strlen(weird), &r), AG_OK);
    AG_CHECK_INT(r.status, 404);

    /* A length that is not a number is no length, not a zero-length body. */
    const char junk_len[] = "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_response(junk_len, strlen(junk_len), &r), AG_OK);
    AG_CHECK(!r.have_length);

    const char not_http[] = "220 ProFTPD\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_response(not_http, strlen(not_http), &r),
                 -AG_EFORMAT);
    const char truncated[] = "HTTP/1.1 2\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_response(truncated, strlen(truncated), &r),
                 -AG_EFORMAT);
}

static void test_http_request(void)
{
    ag_http_req_t q;

    const char get[] = "GET /index.htm HTTP/1.1\r\n"
                       "Host: 10.0.2.15\r\n"
                       "User-Agent: curl/8.5\r\n"
                       "\r\n";
    AG_CHECK_INT(ag_http_parse_request(get, strlen(get), &q), AG_OK);
    AG_CHECK_STR(q.method, "GET");
    AG_CHECK_STR(q.target, "/index.htm");
    AG_CHECK(q.keep_alive);

    /* 1.0 does not keep the connection; 1.1 with Connection: close does not. */
    const char ten[] = "GET / HTTP/1.0\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_request(ten, strlen(ten), &q), AG_OK);
    AG_CHECK(!q.keep_alive);
    const char closing[] = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_request(closing, strlen(closing), &q), AG_OK);
    AG_CHECK(!q.keep_alive);

    /* Lower case verbs are upper-cased so the server compares one thing. */
    const char head[] = "head /a HTTP/1.1\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_request(head, strlen(head), &q), AG_OK);
    AG_CHECK_STR(q.method, "HEAD");

    /* The query is cut, and the target arrives decoded. */
    const char query[] = "GET /dir/my%20file.txt?v=2 HTTP/1.1\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_request(query, strlen(query), &q), AG_OK);
    AG_CHECK_STR(q.target, "/dir/my file.txt");

    /* An encoded '?' is part of the name, not the start of a query. */
    const char pct_q[] = "GET /a%3Fb.txt HTTP/1.1\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_request(pct_q, strlen(pct_q), &q), AG_OK);
    AG_CHECK_STR(q.target, "/a?b.txt");

    const char no_target[] = "GET\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_request(no_target, strlen(no_target), &q),
                 -AG_EFORMAT);
}

static void test_target_safe(void)
{
    AG_CHECK(ag_http_target_safe("/"));
    AG_CHECK(ag_http_target_safe("/a/b/c.txt"));
    AG_CHECK(ag_http_target_safe("/a b.txt"));
    AG_CHECK(ag_http_target_safe("/...hidden"));
    AG_CHECK(ag_http_target_safe("/a..b"));

    AG_CHECK(!ag_http_target_safe("a.txt"));      /* must be absolute */
    AG_CHECK(!ag_http_target_safe("/../etc"));
    AG_CHECK(!ag_http_target_safe("/a/../../b"));
    AG_CHECK(!ag_http_target_safe("/a/.."));
    AG_CHECK(!ag_http_target_safe("/a\\b"));      /* our own separator */
    AG_CHECK(!ag_http_target_safe("/c:/board.cfg"));
    AG_CHECK(!ag_http_target_safe("/a\tb"));
    AG_CHECK(!ag_http_target_safe(NULL));

    /*
     * The traversal that only exists after decoding: "%2e%2e" is "..", and the
     * check has to run on the decoded target for this to be caught at all.
     */
    ag_http_req_t q;
    const char    escaped[] = "GET /%2e%2e/%2e%2e/board.cfg HTTP/1.1\r\n\r\n";
    AG_CHECK_INT(ag_http_parse_request(escaped, strlen(escaped), &q), AG_OK);
    AG_CHECK_STR(q.target, "/../../board.cfg");
    AG_CHECK(!ag_http_target_safe(q.target));
}

static void test_chunk_and_pct(void)
{
    uint64_t n = 0;

    AG_CHECK_INT(ag_http_chunk_size("1a2b\r\n", &n), AG_OK);
    AG_CHECK_INT((long)n, 0x1a2b);
    AG_CHECK_INT(ag_http_chunk_size("0\r\n", &n), AG_OK);
    AG_CHECK_INT((long)n, 0);
    AG_CHECK_INT(ag_http_chunk_size("FF", &n), AG_OK);
    AG_CHECK_INT((long)n, 255);
    AG_CHECK_INT(ag_http_chunk_size("10;name=value\r\n", &n), AG_OK);
    AG_CHECK_INT((long)n, 16);
    AG_CHECK_INT(ag_http_chunk_size("", &n), -AG_EINVAL);
    AG_CHECK_INT(ag_http_chunk_size("zz\r\n", &n), -AG_EINVAL);
    AG_CHECK_INT(ag_http_chunk_size("10 junk\r\n", &n), -AG_EINVAL);

    char a[] = "/my%20file%2Ftxt";
    AG_CHECK_INT((long)ag_pct_decode(a), 12);
    AG_CHECK_STR(a, "/my file/txt");

    /* A stray '%' is itself: eating the bytes after it renames the file. */
    char b[] = "/100%done";
    AG_CHECK_INT((long)ag_pct_decode(b), 9);
    AG_CHECK_STR(b, "/100%done");
    char c[] = "/a%2";
    (void)ag_pct_decode(c);
    AG_CHECK_STR(c, "/a%2");
}

static void test_pct_encode(void)
{
    char out[64];

    AG_CHECK_INT((long)ag_pct_encode("plain.txt", out, sizeof(out)), 9);
    AG_CHECK_STR(out, "plain.txt");

    AG_CHECK(ag_pct_encode("my file.txt", out, sizeof(out)) > 0);
    AG_CHECK_STR(out, "my%20file.txt");

    AG_CHECK(ag_pct_encode("a&b=c?d", out, sizeof(out)) > 0);
    AG_CHECK_STR(out, "a%26b%3Dc%3Fd");

    /* A name that does not fit is not linked to at all. */
    char tiny[6];
    AG_CHECK_INT((long)ag_pct_encode("my file", tiny, sizeof(tiny)), 0);

    /* Encoding then decoding is the identity, which is what a link needs. */
    char round[64];
    AG_CHECK(ag_pct_encode("D&D notes (1).txt", round, sizeof(round)) > 0);
    (void)ag_pct_decode(round);
    AG_CHECK_STR(round, "D&D notes (1).txt");
}

static void test_mime_and_status(void)
{
    AG_CHECK_STR(ag_http_mime("/a/index.htm"), "text/html");
    AG_CHECK_STR(ag_http_mime("/a/INDEX.HTML"), "text/html");
    AG_CHECK_STR(ag_http_mime("readme.txt"), "text/plain");
    AG_CHECK_STR(ag_http_mime("shot.ppm"), "image/x-portable-pixmap");
    AG_CHECK_STR(ag_http_mime("hello.axe"), "application/octet-stream");
    AG_CHECK_STR(ag_http_mime("noext"), "application/octet-stream");
    /* A dot in a directory name is not an extension. */
    AG_CHECK_STR(ag_http_mime("/v1.0/file"), "application/octet-stream");

    AG_CHECK_STR(ag_http_status_text(404), "Not Found");
    AG_CHECK_STR(ag_http_status_text(200), "OK");
    AG_CHECK_STR(ag_http_status_text(599), "");
}

static void test_ftp(void)
{
    bool final = false;

    AG_CHECK_INT(ag_ftp_reply_code("220 ready\r\n", &final), 220);
    AG_CHECK(final);
    AG_CHECK_INT(ag_ftp_reply_code("220-first of many\r\n", &final), 220);
    AG_CHECK(!final);
    AG_CHECK_INT(ag_ftp_reply_code("226\r\n", &final), 226);
    AG_CHECK(final);
    AG_CHECK_INT(ag_ftp_reply_code(" 220 ready", &final), -1);
    AG_CHECK_INT(ag_ftp_reply_code("2200 ready", &final), -1);
    AG_CHECK_INT(ag_ftp_reply_code("hello", &final), -1);

    uint32_t addr = 0;
    uint16_t port = 0;
    AG_CHECK_INT(ag_ftp_pasv_parse(
                     "227 Entering Passive Mode (10,0,2,2,195,149).\r\n", &addr,
                     &port),
                 AG_OK);
    AG_CHECK_INT(addr, 0x0a000202u);
    AG_CHECK_INT(port, 50069);

    /* No parentheses, and a sentence in another language: still six numbers. */
    AG_CHECK_INT(ag_ftp_pasv_parse("227 =127,0,0,1,4,1\r\n", &addr, &port),
                 AG_OK);
    AG_CHECK_INT(addr, 0x7f000001u);
    AG_CHECK_INT(port, 1025);

    /* The reply code itself must not be mistaken for the first number. */
    AG_CHECK_INT(ag_ftp_pasv_parse("227 Passive (192,168,1,10,8,0)", &addr,
                                   &port),
                 AG_OK);
    AG_CHECK_INT(addr, 0xc0a8010au);
    AG_CHECK_INT(port, 2048);

    AG_CHECK_INT(ag_ftp_pasv_parse("227 no numbers here", &addr, &port),
                 -AG_EFORMAT);
    AG_CHECK_INT(ag_ftp_pasv_parse("227 (1,2,3)", &addr, &port), -AG_EFORMAT);
    AG_CHECK_INT(ag_ftp_pasv_parse("227 (1,2,3,4,5,999)", &addr, &port),
                 -AG_EFORMAT);

    uint64_t size = 0;
    AG_CHECK_INT(ag_ftp_size_parse("213 123456\r\n", &size), AG_OK);
    AG_CHECK_INT((long)size, 123456);
    AG_CHECK_INT(ag_ftp_size_parse("213 not a number\r\n", &size), -AG_EINVAL);
    AG_CHECK_INT(ag_ftp_size_parse("550 no such file\r\n", &size), -AG_EINVAL);
    AG_CHECK_INT(ag_ftp_size_parse("213-123\r\n", &size), -AG_EINVAL);
}

void run_netmsg_tests(void)
{
    test_ipv4();
    test_url();
    test_http_header_end();
    test_http_response();
    test_http_request();
    test_target_safe();
    test_chunk_and_pct();
    test_pct_encode();
    test_mime_and_status();
    test_ftp();
}
