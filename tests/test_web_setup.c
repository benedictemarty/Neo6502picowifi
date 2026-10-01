/* test_web_setup.c — page web du point d'accès de configuration (US-W6) : tests sur PC. */
#include "../src/web_setup.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_IN(needle) do { checks++; if (!strstr(resp, needle)) { failures++; printf("ÉCHEC %s:%d : réponse sans « %s » :\n%s\n", __FILE__, __LINE__, needle, resp); } } while (0)
#define CHECK_NOT_IN(needle) do { checks++; if (strstr(resp, needle)) { failures++; printf("ÉCHEC %s:%d : réponse contient « %s »\n", __FILE__, __LINE__, needle); } } while (0)

static struct web_ap aps[3] = { { "Livebox-1234", -50, 3 }, { "<script>\"x\"&", -70, 0 }, { "Free WiFi", -80, 0 } };
static struct web_status st;
static struct web_form form;
static struct web_hosts hosts;
static char resp[6144];
static size_t len;

static enum web_result req(const char *r)
{
    memset(resp, 0, sizeof resp);
    memset(&form, 0, sizeof form);
    memset(&hosts, 0xaa, sizeof hosts);
    enum web_result x = web_setup_handle(r, strlen(r), &st, &form, &hosts, resp, sizeof resp - 1, &len);
    resp[len] = 0;
    return x;
}

static enum web_result post_to(const char *path, const char *body)
{
    char r[2048];
    snprintf(r, sizeof r, "POST %s HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                          "Content-Length: %u\r\n\r\n%s", path, (unsigned)strlen(body), body);
    return req(r);
}

static enum web_result post(const char *body)
{
    char r[2048];
    snprintf(r, sizeof r, "POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                          "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(body), body);
    return req(r);
}

static void reset(void)
{
    memset(&st, 0, sizeof st);
    st.ap_ip = "192.168.4.1";
    st.saved_ssid = "";
    st.sta_ip = "";
    st.version = "0.4.0";
    st.join_ssid = "";
    st.n_ap = 3;
    st.ap = aps;
}

static void test_page(void)
{
    reset();
    CHECK(req("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n") == WEB_REPLY);
    CHECK(!strncmp(resp, "HTTP/1.1 200 OK\r\n", 17));
    CHECK_IN("Content-Type: text/html; charset=utf-8");
    char *body = strstr(resp, "\r\n\r\n") + 4;
    char cl[32];
    snprintf(cl, sizeof cl, "Content-Length: %u\r\n", (unsigned)strlen(body));
    CHECK_IN(cl);
    CHECK_IN("aucun");
    CHECK_IN("value=\"Livebox-1234\"");
    CHECK_IN("Free WiFi <small>(-80 dBm, ouvert)");
    CHECK_IN("&lt;script&gt;&quot;x&quot;&amp;");           /* SSID échappé */
    CHECK_NOT_IN("<script>");
    CHECK_IN("Modem 0.4.0");
    CHECK_NOT_IN("http-equiv=refresh");

    /* réseau mémorisé et connecté ; le mot de passe n'apparaît jamais */
    st.saved_ssid = "Livebox-1234";
    st.sta_ip = "192.168.1.42";
    req("GET / HTTP/1.1\r\n\r\n");                       /* sans Host : accepté */
    CHECK_IN("<b>Livebox-1234</b>");
    CHECK_IN("Connect\xc3\xa9, adresse IP 192.168.1.42");
    CHECK_NOT_IN("value=\"secret");

    /* états d'association */
    st.join_ssid = "Free WiFi";
    st.join = WEB_JOIN_RUNNING; req("GET / HTTP/1.1\r\n\r\n"); CHECK_IN("en cours"); CHECK_IN("http-equiv=refresh");
    st.join = WEB_JOIN_OK; req("GET / HTTP/1.1\r\n\r\n"); CHECK_IN("va se fermer"); CHECK_NOT_IN("http-equiv=refresh");
    st.join = WEB_JOIN_BAD_PASSWORD; req("GET / HTTP/1.1\r\n\r\n"); CHECK_IN("Mot de passe refus");
    st.join = WEB_JOIN_NO_AP; req("GET / HTTP/1.1\r\n\r\n"); CHECK_IN("introuvable");
    st.join = WEB_JOIN_FAIL; req("GET / HTTP/1.1\r\n\r\n"); CHECK_IN("chec de la connexion");
    st.join = WEB_JOIN_IDLE;
    st.scanning = 1; req("GET / HTTP/1.1\r\n\r\n"); CHECK_IN("Recherche en cours"); CHECK_IN("http-equiv=refresh");
    st.scanning = 0; st.n_ap = 0; req("GET / HTTP/1.1\r\n\r\n"); CHECK_IN("Aucun r\xc3\xa9seau trouv");
}

static void test_captive(void)
{
    reset();
    /* autre hôte (détection de portail captif des téléphones) → redirection */
    CHECK(req("GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\n\r\n") == WEB_REPLY);
    CHECK(!strncmp(resp, "HTTP/1.1 302 Found\r\n", 20));
    CHECK_IN("Location: http://192.168.4.1/\r\n");
    req("GET /hotspot-detect.html HTTP/1.1\r\nhost: captive.apple.com\r\n\r\n");
    CHECK_IN("302 Found");
    /* notre hôte (avec :80) ; chemin inconnu → redirection */
    req("GET / HTTP/1.1\r\nHost: 192.168.4.1:80\r\n\r\n"); CHECK_IN("200 OK");
    req("GET /favicon.ico HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n"); CHECK_IN("302 Found");
    /* /scan → relance + redirection ; paramètres ignorés */
    CHECK(req("GET /scan?x=1 HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n") == WEB_REPLY_RESCAN);
    CHECK_IN("302 Found");
    /* requête incomplète */
    CHECK(req("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n") == WEB_INCOMPLETE);
}

static void test_form(void)
{
    reset();
    /* réseau choisi dans la liste, mot de passe encodé */
    CHECK(post("ssid=Livebox-1234&ssid_manual=&pass=mot+de%20passe%26%3D") == WEB_REPLY_SUBMIT);
    CHECK(!strcmp(form.ssid, "Livebox-1234") && !strcmp(form.pass, "mot de passe&="));
    CHECK_IN("200 OK"); CHECK_IN("en cours"); CHECK_IN("Livebox-1234");
    CHECK_NOT_IN("mot de passe&amp;");
    /* nom saisi prioritaire ; réseau ouvert (mot de passe vide) */
    CHECK(post("ssid=Livebox-1234&ssid_manual=Mon+r%C3%A9seau&pass=") == WEB_REPLY_SUBMIT);
    CHECK(!strcmp(form.ssid, "Mon r\xc3\xa9seau") && form.pass[0] == 0);
    /* erreurs : aucun réseau, mot de passe trop court / trop long, nom trop long */
    CHECK(post("ssid_manual=&pass=12345678") == WEB_REPLY); CHECK_IN("400 Bad Request"); CHECK_IN("Choisissez un r");
    CHECK(post("ssid=X&pass=1234567") == WEB_REPLY); CHECK_IN("de 8 \xc3\xa0 63");
    char body[256] = "ssid=X&pass=";
    memset(body + strlen(body), 'a', 64);
    CHECK(post(body) == WEB_REPLY); CHECK_IN("de 8 \xc3\xa0 63");
    strcpy(body, "ssid_manual=");
    memset(body + strlen(body), 'n', 33);
    CHECK(post(body) == WEB_REPLY); CHECK_IN("trop long");
    strcpy(body, "ssid_manual=");
    memset(body + strlen(body), 'n', 32);
    body[12 + 32] = 0;
    strcat(body, "&pass=12345678");
    CHECK(post(body) == WEB_REPLY_SUBMIT && strlen(form.ssid) == 32);
    /* corps incomplet, sans longueur, trop grand */
    CHECK(req("POST /save HTTP/1.1\r\nContent-Length: 20\r\n\r\nssid=X") == WEB_INCOMPLETE);
    CHECK(req("POST /save HTTP/1.1\r\n\r\nssid=X") == WEB_REPLY); CHECK_IN("411");
    CHECK(req("POST /save HTTP/1.1\r\nContent-Length: 5000\r\n\r\n") == WEB_REPLY); CHECK_IN("413");
    /* GET /save : pas d'enregistrement */
    CHECK(req("GET /save?ssid=X HTTP/1.1\r\n\r\n") == WEB_REPLY); CHECK_IN("302");
}

static void test_overflow(void)
{
    reset();
    char small[256];
    struct web_form f;
    struct web_hosts h;
    size_t n;
    const char *r = "GET / HTTP/1.1\r\n\r\n";
    CHECK(web_setup_handle(r, strlen(r), &st, &f, &h, small, sizeof small, &n) == WEB_REPLY);
    CHECK(n > 0 && n < sizeof small && !strncmp(small, "HTTP/1.1 500", 12));
    /* liste complète de réseaux à SSID de 32 caractères spéciaux : tient dans 6 Ko */
    static struct web_ap many[WEB_AP_MAX];
    for (int i = 0; i < WEB_AP_MAX; i++) { memset(many[i].ssid, '&', 32); many[i].ssid[32] = 0; many[i].rssi = -90; many[i].ecn = 3; }
    st.ap = many; st.n_ap = WEB_AP_MAX;
    req("GET / HTTP/1.1\r\n\r\n");
    CHECK(!strncmp(resp, "HTTP/1.1 200 OK", 15));
    CHECK_IN("</html>");                                  /* liste écourtée, page complète */
    CHECK_IN("<p>\xe2\x80\xa6</p>");
}

/* US-T12 : hôtes autorisés et journal */
static char cfg_hosts[AT_HOSTS_MAX][AT_HOST_MAX + 1];
static struct at_log_entry logs[AT_LOG_MAX];

static void test_hosts(void)
{
    reset();
    memset(cfg_hosts, 0, sizeof cfg_hosts);
    strcpy(cfg_hosts[0], "tnfs.example");
    strcpy(cfg_hosts[3], "*.neonav.fr");
    st.hosts = (const char (*)[AT_HOST_MAX + 1])cfg_hosts;
    st.hosts_enforce = 1;
    st.now_ms = 100000;
    logs[0] = (struct at_log_entry){ 99000, "TCP", "tnfs.example", 16384, true };
    logs[1] = (struct at_log_entry){ 40000, "PING", "<b>evil", 0, false };
    st.log[0] = &logs[0]; st.log[1] = &logs[1]; st.n_log = 2;

    req("GET / HTTP/1.1\r\n\r\n"); CHECK_IN("<a href=/hosts>");
    CHECK(req("GET /hosts HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n") == WEB_REPLY);
    CHECK_IN("200 OK"); CHECK_IN("value=1 checked>");
    CHECK_IN("<textarea name=hosts rows=8>tnfs.example\n*.neonav.fr\n</textarea>");
    CHECK_IN("<td>TCP</td><td>tnfs.example:16384</td><td class=o>autoris");
    CHECK_IN("il y a 1 s"); CHECK_IN("il y a 60 s");
    CHECK_IN("&lt;b&gt;evil</td><td class=e>refus");          /* nom journalisé échappé */
    CHECK_NOT_IN("<b>evil");
    st.hosts_enforce = 0; st.n_log = 0;
    req("GET /hosts HTTP/1.1\r\n\r\n"); CHECK_NOT_IN("checked"); CHECK_IN("Aucune.");

    /* enregistrement : lignes CRLF, espaces, lignes vides ignorées */
    CHECK(post_to("/hosts", "enforce=1&hosts=+tnfs.example+%0D%0A%0D%0A*.neonav.fr%0D%0A192.168.1.10") == WEB_REPLY_HOSTS);
    CHECK(hosts.enforce == 1 && !strcmp(hosts.hosts[0], "tnfs.example") && !strcmp(hosts.hosts[1], "*.neonav.fr")
          && !strcmp(hosts.hosts[2], "192.168.1.10") && hosts.hosts[3][0] == 0 && hosts.hosts[7][0] == 0);
    CHECK_IN("Liste enregistr"); CHECK_IN("checked");
    CHECK_IN(">tnfs.example\n*.neonav.fr\n192.168.1.10\n</textarea>");
    /* case décochée : filtrage désactivé, liste gardée */
    CHECK(post_to("/hosts", "hosts=a.fr") == WEB_REPLY_HOSTS && hosts.enforce == 0 && !strcmp(hosts.hosts[0], "a.fr"));
    CHECK(post_to("/hosts", "enforce=1&hosts=") == WEB_REPLY_HOSTS && hosts.enforce == 1 && hosts.hosts[0][0] == 0);
    /* erreurs : entrée invalide, trop d'entrées → rien n'est enregistré */
    CHECK(post_to("/hosts", "enforce=1&hosts=bad%20host") == WEB_REPLY); CHECK_IN("400"); CHECK_IN("invalide");
    CHECK(post_to("/hosts", "hosts=*evil.fr") == WEB_REPLY); CHECK_IN("invalide");
    CHECK(post_to("/hosts", "hosts=a%0Ab%0Ac%0Ad%0Ae%0Af%0Ag%0Ah%0Ai") == WEB_REPLY); CHECK_IN("8 h");
    CHECK(post_to("/hosts", "hosts=a%0Ab%0Ac%0Ad%0Ae%0Af%0Ag%0Ah") == WEB_REPLY_HOSTS && !strcmp(hosts.hosts[7], "h"));
    /* autre hôte HTTP : redirection, pas d'enregistrement */
    CHECK(req("POST /hosts HTTP/1.1\r\nHost: evil.example\r\nContent-Length: 9\r\n\r\nenforce=0") == WEB_REPLY);
    CHECK_IN("302 Found");
    /* journal complet de noms à échapper : page complète */
    static struct at_log_entry big[AT_LOG_MAX];
    for (int i = 0; i < AT_LOG_MAX; i++) {
        big[i] = (struct at_log_entry){ 0, "TCP", "", 65535, false };
        memset(big[i].host, '&', AT_HOST_MAX); big[i].host[AT_HOST_MAX] = 0;
        st.log[i] = &big[i];
    }
    st.n_log = AT_LOG_MAX;
    for (int i = 0; i < AT_HOSTS_MAX; i++) { memset(cfg_hosts[i], 'x', AT_HOST_MAX); cfg_hosts[i][AT_HOST_MAX] = 0; }
    req("GET /hosts HTTP/1.1\r\n\r\n"); CHECK_IN("200 OK"); CHECK_IN("</html>");
}

int main(void)
{
    test_page();
    test_captive();
    test_form();
    test_overflow();
    test_hosts();
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
