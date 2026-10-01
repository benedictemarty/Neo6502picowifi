/*
 * web_setup.h — page web du point d'accès de configuration (US-W6).
 *
 * Portable : une requête HTTP en entrée, la réponse complète en sortie ;
 * le transport (lwIP) est dans ap_pico.c. Testé sur PC (tests/test_web_setup.c).
 *  GET  /       état, réseaux trouvés, formulaire
 *  GET  /scan   relance la recherche des réseaux, puis redirige vers /
 *  POST /save   ssid, ssid_manual (prioritaire s'il est rempli), pass
 *  GET  /hosts  hôtes autorisés et journal des connexions (US-T12)
 *  POST /hosts  enforce (case cochée = 1), hosts (un par ligne) ; seul moyen
 *               de modifier la liste (en AT elle est en lecture seule)
 *  autre chemin, ou autre Host que l'adresse du point d'accès : redirection
 *  vers http://<ap_ip>/ (portail captif des téléphones)
 * Le mot de passe enregistré n'est jamais renvoyé dans la page.
 */
#ifndef WEB_SETUP_H
#define WEB_SETUP_H

#include <stddef.h>
#include <stdint.h>

#include "at_modem.h"

#define WEB_AP_MAX 24

struct web_ap {
    char ssid[33];
    int  rssi;
    int  ecn;            /* 0 = ouvert (codes ESP, comme AT+CWLAP) */
};

enum web_join {
    WEB_JOIN_IDLE = 0,
    WEB_JOIN_RUNNING,
    WEB_JOIN_OK,
    WEB_JOIN_BAD_PASSWORD,
    WEB_JOIN_NO_AP,
    WEB_JOIN_FAIL,
};

struct web_status {
    const char *ap_ip;        /* "192.168.4.1"                       */
    const char *saved_ssid;   /* réseau mémorisé, "" = aucun         */
    const char *sta_ip;       /* IP obtenue, "" si non associé       */
    const char *version;
    enum web_join join;
    const char *join_ssid;    /* réseau de la dernière tentative     */
    int scanning;
    int n_ap;
    const struct web_ap *ap;
    /* US-T12 */
    int hosts_enforce;
    const char (*hosts)[AT_HOST_MAX + 1];      /* AT_HOSTS_MAX entrées, "" = libre */
    const struct at_log_entry *log[AT_LOG_MAX]; /* plus récente d'abord          */
    int n_log;
    uint32_t now_ms;
};

struct web_hosts {
    int  enforce;
    char hosts[AT_HOSTS_MAX][AT_HOST_MAX + 1];
};

struct web_form {
    char ssid[33];
    char pass[65];
};

enum web_result {
    WEB_INCOMPLETE = 0,   /* requête pas encore complète : attendre la suite */
    WEB_REPLY,            /* réponse prête                                   */
    WEB_REPLY_SUBMIT,     /* réponse prête + form à enregistrer et rejoindre */
    WEB_REPLY_RESCAN,     /* réponse prête + relancer la recherche           */
    WEB_REPLY_HOSTS,      /* réponse prête + hosts à enregistrer             */
};

enum web_result web_setup_handle(const char *req, size_t len, const struct web_status *st,
                                 struct web_form *form, struct web_hosts *hosts,
                                 char *resp, size_t cap, size_t *resp_len);

#endif
