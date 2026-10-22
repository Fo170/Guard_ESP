// ════════════════════════════════════════════════════════════════
// Guard_ESP_web — filtre TOUTES les routes web
//
// ⚠️ ANGLE MORT COMBLÉ ICI : le contrôle de flux de Server_MCP ne couvre
//    que `tools/call`. `initialize`, `tools/list`, `ping` et surtout
//    l'interface web `/` restaient GRATUITS et ILLIMITÉS. Mesuré : une
//    seule requête web bloque le loop 10 s sur un ESP8266 (Surveillance
//    Batterie) alors qu'aucun 429 ne se déclenche.
//
// Deux adaptateurs :
//   GE_WebGuard        → ESP8266WebServer / WebServer (synchrone)
//   GE_AsyncWebGuard   → AsyncWebServer (Coulometre)
//
// Usage synchrone (une ligne au début de CHAQUE handler) :
//   if (!wguard.admit(server, 1, 400)) return;   // refus déjà envoyé
//   ... traitement ...
//   wguard.fin();
// ════════════════════════════════════════════════════════════════

#ifndef GUARD_ESP_WEB_H
#define GUARD_ESP_WEB_H

#include <Arduino.h>
#include "Guard_ESP.h"

// ────────────────────────────────────────────────────────────────
// Code HTTP selon la cause de refus
//   429 Too Many Requests → rafale / backoff (le client peut réessayer)
//   503 Service Unavailable → concurrence / pression / garde (patienter)
// ────────────────────────────────────────────────────────────────
static inline uint16_t ge_codeHttp(GE_Refus r) {
  switch (r) {
    case GE_REFUS_FLUX:
    case GE_REFUS_POIDS:
    case GE_REFUS_BACKOFF:
      return 429;
    default:
      return 503;
  }
}

static inline const char* ge_messageRefus(GE_Refus r) {
  switch (r) {
    case GE_REFUS_FLUX:        return "Trop de requetes — fenetre saturee";
    case GE_REFUS_POIDS:       return "Requete trop lourde pour la fenetre de flux";
    case GE_REFUS_PRESSION:    return "Memoire insuffisante (XOFF) — patientez";
    case GE_REFUS_CONCURRENCE: return "Trop de requetes simultanees — patientez";
    case GE_REFUS_BODY:        return "Reponse trop volumineuse pour la memoire";
    case GE_REFUS_BACKOFF:     return "Service en cooldown — patientez";
    case GE_REFUS_GARDE:       return "Appareil non pret (memoire insuffisante)";
    case GE_REFUS_DUREE:       return "Requete precedente trop longue";
    default:                   return "Refuse";
  }
}

// ════════════════════════════════════════════════════════════════
// Adaptateur SYNCHRONE : ESP8266WebServer / WebServer
// ════════════════════════════════════════════════════════════════
template <typename ServerType>
class GE_WebGuard {
public:
  explicit GE_WebGuard(Guard_ESP& g) : _g(g) {}

  // Retourne true = la requête est admise (slot acquis, à rendre via fin()).
  bool admit(ServerType& srv, uint8_t poids = 1, uint32_t taille = 0) {
    _ouvert = false;
    if (_g.admit(poids, taille)) { _ouvert = true; return true; }
    refuser(srv);
    return false;
  }

  // Envoie la réponse de refus (429/503 + Retry-After + X-RateLimit-*).
  void refuser(ServerType& srv) {
    GE_Refus r = _g.refus();
    uint16_t code = ge_codeHttp(r);
    char buf[24];

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)((_g.retryAfterMs() + 999) / 1000));
    srv.sendHeader("Retry-After", String(buf));

    snprintf(buf, sizeof(buf), "%u", (unsigned)_g.canaux());
    srv.sendHeader("X-RateLimit-Limit", String(buf));
    snprintf(buf, sizeof(buf), "%u", (unsigned)_g.chargeCourante());
    srv.sendHeader("X-RateLimit-Used", String(buf));
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(_g.fenetreMs() / 1000));
    srv.sendHeader("X-RateLimit-Window", String(buf));
    srv.sendHeader("X-Guard-Reason", ge_messageRefus(r));

    srv.send(code, "text/plain", ge_messageRefus(r));
  }

  // À appeler en fin de handler admis : rend le slot et mesure la durée.
  void fin() {
    if (_ouvert) { _g.sortir(); _ouvert = false; }
  }

  Guard_ESP& guard() { return _g; }

private:
  Guard_ESP& _g;
  bool _ouvert = false;
};

// ════════════════════════════════════════════════════════════════
// Adaptateur ASYNCHRONE : AsyncWebServer
// ════════════════════════════════════════════════════════════════
template <typename RequestType>
class GE_AsyncWebGuard {
public:
  explicit GE_AsyncWebGuard(Guard_ESP& g) : _g(g) {}

  // Retourne true = admis. En cas de refus, la réponse est déjà envoyée.
  bool admit(RequestType* req, uint8_t poids = 1, uint32_t taille = 0) {
    _ouvert = false;
    if (_g.admit(poids, taille)) { _ouvert = true; return true; }
    refuser(req);
    return false;
  }

  void refuser(RequestType* req) {
    GE_Refus r = _g.refus();
    uint16_t code = ge_codeHttp(r);
    char buf[24];

    typename RequestType::Response* resp = req->beginResponse(code, "text/plain", ge_messageRefus(r));
    if (resp) {
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)((_g.retryAfterMs() + 999) / 1000));
      resp->addHeader("Retry-After", String(buf));
      snprintf(buf, sizeof(buf), "%u", (unsigned)_g.canaux());
      resp->addHeader("X-RateLimit-Limit", String(buf));
      snprintf(buf, sizeof(buf), "%u", (unsigned)_g.chargeCourante());
      resp->addHeader("X-RateLimit-Used", String(buf));
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)(_g.fenetreMs() / 1000));
      resp->addHeader("X-RateLimit-Window", String(buf));
      resp->addHeader("X-Guard-Reason", ge_messageRefus(r));
      req->send(resp);
    } else {
      req->send(code, "text/plain", ge_messageRefus(r));
    }
  }

  void fin() {
    if (_ouvert) { _g.sortir(); _ouvert = false; }
  }

  Guard_ESP& guard() { return _g; }

private:
  Guard_ESP& _g;
  bool _ouvert = false;
};

#endif // GUARD_ESP_WEB_H
