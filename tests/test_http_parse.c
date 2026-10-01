/* test_http_parse.c — URL, redirections, en-têtes, « chunked » (US-T11) : tests sur PC. */
#include "../src/http_parse.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_url(void)
{
    struct http_url u, r;
    CHECK(http_url_parse("http://example.com", &u) && !u.https && !strcmp(u.host, "example.com") && u.port == 80 && !strcmp(u.path, "/"));
    CHECK(http_url_parse("HTTPS://a.b-c.fr:8443/x/y?q=1#frag", &u) && u.https && u.port == 8443 && !strcmp(u.path, "/x/y?q=1"));
    CHECK(http_url_parse("https://h.fr?x", &u) && !strcmp(u.path, "/?x") && u.port == 443);
    CHECK(!http_url_parse("ftp://x", &u) && !http_url_parse("http://", &u) && !http_url_parse("http://:80/", &u));
    CHECK(!http_url_parse("http://h:0/", &u) && !http_url_parse("http://h:99999/", &u) && !http_url_parse("http://h:8x/", &u));
    CHECK(!http_url_parse("http://bad host/", &u) && !http_url_parse("http://h/a b", &u) && !http_url_parse("http://u@h/", &u));
    char longp[300] = "http://h/";
    memset(longp + 9, 'p', 250); longp[259] = 0;
    CHECK(!http_url_parse(longp, &u));
    /* redirections */
    http_url_parse("https://site.fr:444/a", &u);
    CHECK(http_url_resolve(&u, "/b?c", &r) && r.https && !strcmp(r.host, "site.fr") && r.port == 444 && !strcmp(r.path, "/b?c"));
    CHECK(http_url_resolve(&u, "//cdn.fr/z", &r) && r.https && !strcmp(r.host, "cdn.fr") && r.port == 443);
    CHECK(http_url_resolve(&u, "http://other.fr/", &r) && !r.https && r.port == 80);
    CHECK(!http_url_resolve(&u, "relatif.html", &r) && !http_url_resolve(&u, "/a b", &r));
}

static bool parse(const char *h, struct http_resp *r) { return http_resp_parse(h, strlen(h), r); }

static void test_resp(void)
{
    struct http_resp r;
    CHECK(parse("HTTP/1.1 200 OK\r\nContent-Length: 42\r\ncontent-type: text/html; charset=\"utf-8\"\r\n\r\n", &r));
    CHECK(r.status == 200 && r.content_length == 42 && !r.chunked && !strcmp(r.content_type, "text/html; charset='utf-8'"));
    CHECK(parse("HTTP/1.0 301 Moved\r\nLocation:  https://x.fr/  \r\n\r\n", &r) && http_is_redirect(&r) && !strcmp(r.location, "https://x.fr/"));
    CHECK(parse("HTTP/1.1 302 Found\r\n\r\n", &r) && !http_is_redirect(&r));               /* sans Location */
    CHECK(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\nContent-Length: 5\r\n\r\n", &r) && r.chunked && r.content_length == -1);
    CHECK(parse("HTTP/1.1 200 OK\r\n\r\n", &r) && r.content_length == -1);
    CHECK(!parse("HTTP/1.1 200 OK\r\nContent-Length: -3\r\n\r\n", &r));
    CHECK(!parse("HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n", &r));
    CHECK(!parse("HTTP/2 200\r\n\r\n", &r) && !parse("ICY 200 OK\r\n\r\n", &r) && !parse("HTTP/1.1 2x0 OK\r\n\r\n", &r));
    CHECK(!http_resp_parse("HTTP/1.1 200 OK", 15, &r));
    /* Location très long : tronqué sans débordement */
    static char h[1200];
    char *p = h + sprintf(h, "HTTP/1.1 302 Found\r\nLocation: http://x/");
    memset(p, 'a', 900); p += 900;
    strcpy(p, "\r\n\r\n");
    CHECK(parse(h, &r) && strlen(r.location) == sizeof r.location - 1);
}

static size_t decode(const char *in, char *out, struct http_chunked *c)
{
    size_t n = 0;
    http_chunked_init(c);
    for (const char *s = in; *s; s++) if (http_chunked_feed(c, (uint8_t)*s)) out[n++] = *s;
    out[n] = 0;
    return n;
}

static void test_chunked(void)
{
    struct http_chunked c;
    char out[256];
    CHECK(decode("4\r\nWiki\r\n5;name=v\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\n\r\n", out, &c) == 23);
    CHECK(!strcmp(out, "Wikipedia in\r\n\r\nchunks.") && c.done && !c.error);
    CHECK(decode("1a\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\nTrailer: x\r\nAutre: y\r\n\r\n", out, &c) == 26 && c.done);
    CHECK(decode("3\r\nabc\r\n", out, &c) == 3 && !c.done && !c.error);              /* incomplet */
    decode("z\r\n", out, &c); CHECK(c.error);
    decode("3\r\nabcX", out, &c); CHECK(c.error);                                     /* CRLF manquant */
    decode("3\rX", out, &c); CHECK(c.error);
    decode("fffffffff\r\n", out, &c); CHECK(c.error);                               /* taille démesurée */
    /* après la fin : plus aucun octet de corps */
    decode("0\r\n\r\n", out, &c);
    CHECK(c.done && !http_chunked_feed(&c, 'x'));
}

int main(void)
{
    test_url();
    test_resp();
    test_chunked();
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
