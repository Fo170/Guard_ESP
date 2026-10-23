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
// Usage (RAII, sûr en concurrence) :
//   GE_Ticket t = wguard.admit(&server, 2, 192);
//   if (!t) return;              // refus déjà envoyé (429/503)
//   ... traitement ...
//   // libération automatique à la sortie de portée
//
// L'ancien couple admit()/fin() stockait l'état « ouvert » dans un booléen
// PAR INSTANCE : deux requêtes simultanées l'écrasaient et un slot de
// concurrence était perdu (mesuré : actifs=1 après 2 admits + 2 fins).
// ════════════════════════════════════════════════════════════════

#ifndef GUARD_ESP_WEB_H
#define GUARD_ESP_WEB_H

#include <Arduino.h>
#include "Guard_ESP.h"

// ────────────────────────────────────────────────────────────────
// Code HTTP selon la cause de refus
//   429 Too Many Requests   → rafale (le client peut réessayer plus tard)
//   503 Service Unavailable → refus permanent ou ressource indisponible
// ⚠️ POIDS est un refus STRUCTUREL (l'outil est trop lourd pour l'état de
//    flux) : réessayer ne changera rien → 503, pas 429.
// ────────────────────────────────────────────────────────────────
static inline uint16_t ge_codeHttp(GE_Refus r) {
  switch (r) {
    case GE_REFUS_FLUX:
    case GE_REFUS_BACKOFF:
      return 429;
    default:
      return 503;
  }
}

static inline const char* ge_messageRefus(GE_Refus r) {
  switch (r) {
    case GE_REFUS_FLUX:        return "Trop de requetes - fenetre saturee";
    case GE_REFUS_POIDS:       return "Requete trop lourde pour la fenetre de flux";
    case GE_REFUS_PRESSION:    return "Memoire insuffisante (XOFF) - patientez";
    case GE_REFUS_CONCURRENCE: return "Trop de requetes simultanees - patientez";
    case GE_REFUS_BODY:        return "Reponse trop volumineuse pour la memoire";
    case GE_REFUS_BACKOFF:     return "Service en cooldown - patientez";
    case GE_REFUS_GARDE:       return "Appareil non pret (memoire insuffisante)";
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

  // Retourne un ticket : convertible en bool, libère le slot à sa destruction.
  GE_Ticket admit(ServerType& srv, uint8_t poids = 1, uint32_t taille = 0) {
    GE_Ticket t = _g.ticket(poids, taille);
    if (!t) refuser(srv);
    return t;
  }

  // Envoie la réponse de refus (429/503 + Retry-After + X-RateLimit-*).
  void refuser(ServerType& srv) {
    GE_Refus r = _g.refus();
    uint16_t code = ge_codeHttp(r);
    char buf[16];

    // Les en-têtes coûtent une String chacun : on ne les émet PAS quand
    // l'appareil est déjà sous pression mémoire — c'est exactement le
    // moment où allouer aggraverait le problème.
    if (!_g.enPression()) {
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)((_g.retryAfterMs() + 999UL) / 1000UL));
      srv.sendHeader("Retry-After", String(buf));

      snprintf(buf, sizeof(buf), "%u", (unsigned)_g.canaux());
      srv.sendHeader("X-RateLimit-Limit", String(buf));

      snprintf(buf, sizeof(buf), "%u", (unsigned)_g.chargeCourante());
      srv.sendHeader("X-RateLimit-Used", String(buf));

      snprintf(buf, sizeof(buf), "%u", (unsigned)_g.creditRestant());
      srv.sendHeader("X-RateLimit-Remaining", String(buf));

      snprintf(buf, sizeof(buf), "%lu", (unsigned long)(_g.fenetreMs() / 1000UL));
      srv.sendHeader("X-RateLimit-Window", String(buf));

      srv.sendHeader("X-Guard-Reason", ge_messageRefus(r));
    }
    srv.send(code, "text/plain", ge_messageRefus(r));
  }

  Guard_ESP& guard() { return _g; }

private:
  Guard_ESP& _g;
};

// ════════════════════════════════════════════════════════════════
// Adaptateur ASYNCHRONE : AsyncWebServer
// Le type de réponse dépend du serveur : on utilise `auto*` pour ne pas
// nommer un type imbriqué qui n'existe pas (l'ancienne version écrivait
// `typename RequestType::Response`, ce qui ne compilait jamais).
// ════════════════════════════════════════════════════════════════
template <typename RequestType>
class GE_AsyncWebGuard {
public:
  explicit GE_AsyncWebGuard(Guard_ESP& g) : _g(g) {}

  GE_Ticket admit(RequestType* req, uint8_t poids = 1, uint32_t taille = 0) {
    GE_Ticket t = _g.ticket(poids, taille);
    if (!t) refuser(req);
    return t;
  }

  void refuser(RequestType* req) {
    GE_Refus r = _g.refus();
    uint16_t code = ge_codeHttp(r);
    char buf[16];

    if (Guard_ESP::heapLibre() > _g.heapXoff() * 2UL) {
      auto* resp = req->beginResponse(code, "text/plain", ge_messageRefus(r));
      if (resp) {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)((_g.retryAfterMs() + 999UL) / 1000UL));
        resp->addHeader("Retry-After", String(buf));
        snprintf(buf, sizeof(buf), "%u", (unsigned)_g.canaux());
        resp->addHeader("X-RateLimit-Limit", String(buf));
        snprintf(buf, sizeof(buf), "%u", (unsigned)_g.chargeCourante());
        resp->addHeader("X-RateLimit-Used", String(buf));
        snprintf(buf, sizeof(buf), "%u", (unsigned)_g.creditRestant());
        resp->addHeader("X-RateLimit-Remaining", String(buf));
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)(_g.fenetreMs() / 1000UL));
        resp->addHeader("X-RateLimit-Window", String(buf));
        resp->addHeader("X-Guard-Reason", ge_messageRefus(r));
        req->send(resp);
        return;
      }
    }
    req->send(code, "text/plain", ge_messageRefus(r));
  }

  Guard_ESP& guard() { return _g; }

private:
  Guard_ESP& _g;
};

#endif // GUARD_ESP_WEB_H
