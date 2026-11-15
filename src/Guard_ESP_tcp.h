// ════════════════════════════════════════════════════════════════
// Guard_ESP_tcp — plafond de connexions AU NIVEAU TCP
//
// ⚠️ ANGLE MORT COMBLÉ ICI : Guard_ESP (noyau) et Guard_ESP_web décident
//    DANS le handler, c'est-à-dire APRÈS que le framework a créé un
//    AsyncWebServerRequest. Or c'est précisément cette création qui tue un
//    ESP8266 sous pression : mesuré le 02/10/2026 sur le Coulométre —
//    allocation de 32 o refusée dans AsyncWebServerRequest::_parseReqHeader()
//    → std::bad_alloc → panic → reboot loop toutes les 5-20 s.
//    Un refus qui a lieu DANS le handler est déjà trop tard.
//
//    Ce module décide dans `onClient()`, donc AVANT toute allocation :
//    une connexion refusée ici ne crée AUCUN AsyncWebServerRequest.
//
// Deux refus, dans cet ordre :
//   1. garde mémoire (Guard_ESP::gardePassee) → 503 + Retry-After
//      « le service s'arrête tant que la RAM est insuffisante »
//   2. plafond de connexions simultanées      → 503 + Retry-After
//
// ⚠️ CONTRAINTES DE CONCEPTION (ne pas régresser)
//   - ZÉRO allocation sur le chemin de refus : ni String, ni new, ni malloc.
//     L'en-tête de refus est construit dans un tampon de PILE (snprintf).
//   - Reproduire EXACTEMENT le comportement d'origine du framework quand la
//     connexion est admise (cf. AsyncWebServer::AsyncWebServer) :
//         c->setRxTimeout(_rxTimeout)
//         RequestType::create(server, client)
//         si create() échoue → c->abort() puis delete c
//     Oublier setRxTimeout() laisse des connexions zombies ; oublier
//     abort()/delete libère le compteur sans fermer le socket.
//   - Sur refus : close(true) — IMMÉDIAT (_close() → tcp_close → _pcb=NULL)
//     PUIS delete. close(false) est DIFFÉRÉ : le delete provoquerait un
//     use-after-free. C'est le défaut le plus coûteux à diagnostiquer.
//   - Le compteur est rendu sur TOUS les chemins (refus excepté, où aucune
//     connexion n'a été comptée) : échec d'allocation ET déconnexion.
//
// ⚠️ PARAMÉTRAGE DES TYPES (aucun #include du framework ici)
//   Ce header ne dépend ni d'ESPAsyncWebServer ni d'ESP8266WebServer : les
//   types sont des PARAMÈTRES de gabarit, résolus à l'instanciation. Cela
//   évite le conflit d'enum HTTPMethod (pitfall 10 du Coulométre) et permet
//   de tester le module sur l'hôte avec de faux serveurs.
//
//   ServerType   : expose onClient(callback, void* arg)
//   ClientType   : expose setRxTimeout(n), write(const char*, size_t),
//                  close(bool), abort()
//   RequestType  : expose static create(ServerType*, ClientType*) et
//                  onDisconnect(callback)
//
//   Exemple (ESPAsyncWebServer) :
//     #include <Guard_ESP.h>
//     #include <Guard_ESP_tcp.h>
//     GE_TcpGuard<AsyncWebServer, AsyncClient, AsyncWebServerRequest>
//         tcp(guard, server, 3);
//     tcp.installer();   // APRÈS le constructeur du serveur (il REMPLACE
//                        // le handler onClient posé par le framework)
//
//   ⚠️ Le garde doit vivre aussi longtemps que le serveur : les callbacks
//      `onDisconnect` capturent son adresse. Instances globales ou
//      membres — jamais un objet de pile dans setup().
//
// Auteur : Olivier FOURNET · Licence : GPL-3.0-only
// ════════════════════════════════════════════════════════════════

#ifndef GUARD_ESP_TCP_H
#define GUARD_ESP_TCP_H

#include <Arduino.h>
#include <stdio.h>
#include <functional>
#include "Guard_ESP.h"

// Plafond par défaut : 3 (un navigateur ouvre plusieurs connexions TCP pour
// UNE page : HTML + script + favicon + polling → 3 ≈ 1 onglet actif).
#ifndef GE_TCP_MAX_CONN_DEF
#define GE_TCP_MAX_CONN_DEF 3
#endif

// Délai de réception par connexion (s). Valeur du framework : 3 s.
#ifndef GE_TCP_RX_TIMEOUT
#define GE_TCP_RX_TIMEOUT 3
#endif

template <typename ServerType, typename ClientType, typename RequestType>
class GE_TcpGuard {
public:
  GE_TcpGuard(Guard_ESP& g, ServerType& srv, uint8_t maxConn = GE_TCP_MAX_CONN_DEF)
    : _g(g), _srv(srv) {
    setMaxConnexions(maxConn);
  }

  // ── Configuration ────────────────────────────────────────────
  // Un plafond de 0 n'a pas de sens : il couperait TOUT le service, y
  // compris l'OTA. On retombe sur 1, qui laisse toujours passer une requête.
  void setMaxConnexions(uint8_t n) { _maxConn = (n == 0) ? 1 : n; }

  // Arrêt du service quand la mémoire est insuffisante (Guard_ESP::setGarde).
  // Activé par défaut : c'est le comportement demandé (ne pas servir plutôt
  // que de planter). Désactivable pour un diagnostic ponctuel.
  void setGardeMemoire(bool on) { _gardeMemoire = on; }

  // Remplace le handler onClient du framework.
  // Surcharge 1 : le serveur expose LUI-MÊME onClient(cb, arg) (ESP8266WebServer,
  //   WebServer, ou un wrapper maison qui ré-expose le handler).
  void installer() {
    _srv.onClient([this](void* /*arg*/, ClientType* c) { entrant(c); }, this);
  }

  // Surcharge 2 : le serveur NE ré-expose PAS onClient — c'est le cas
  // d'AsyncWebServer, où le handler vit sur le membre interne `_server`
  // (protégé). On fournit alors l'ENREGISTREUR, qui reçoit le callback à
  // installer. Exemple (projet Coulométre) :
  //
  //   // classe dérivée qui ré-expose le handler protégé
  //   void onClientConn(AcConnectHandler cb) { _server.onClient(cb, this); }
  //
  //   tcpGuard.installer([](AcConnectHandler cb) { server.onClientConn(cb); });
  //
  // Motif de cette séparation : Guard_ESP_tcp reste SANS dépendance au
  // framework (aucun #include d'ESPAsyncWebServer), donc compilable et
  // testable partout.
  template <typename Enregistreur>
  void installer(Enregistreur enr) {
    enr([this](void* /*arg*/, ClientType* c) { entrant(c); });
  }

  // ── Lecture (diagnostic) ─────────────────────────────────────
  uint8_t  actifs()        const { return _actifs; }
  uint8_t  maxConnexions() const { return _maxConn; }
  uint8_t  pic()           const { return _pic; }
  uint32_t admis()         const { return _admis; }
  uint32_t refuses()       const { return _refuses; }
  uint32_t echecsAlloc()   const { return _echecsAlloc; }
  uint16_t refusGarde()    const { return _rejGarde; }
  uint16_t refusConc()     const { return _rejConc; }
  bool     sature()        const { return _actifs >= _maxConn; }

  // Cause du DERNIER refus TCP (GE_REFUS_AUCUN si aucun).
  GE_Refus refus() const { return _refus; }

  // Résumé texte SANS String (retour ≤ len-1). Même convention que
  // Guard_ESP::resume() : l'appelant fournit le tampon.
  size_t resume(char* buf, size_t len) const {
    if (!buf || len == 0) return 0;
    int n = snprintf(buf, len,
                     "tcp actifs=%u/%u pic=%u admis=%lu refus=%lu "
                     "garde=%u conc=%u alloc=%lu",
                     (unsigned)_actifs, (unsigned)_maxConn, (unsigned)_pic,
                     (unsigned long)_admis, (unsigned long)_refuses,
                     (unsigned)_rejGarde, (unsigned)_rejConc,
                     (unsigned long)_echecsAlloc);
    if (n < 0) { buf[0] = '\0'; return 0; }
    return ((size_t)n < len) ? (size_t)n : (len - 1);
  }

private:
  // ── Décision ─────────────────────────────────────────────────
  // ORDRE VOLONTAIRE : la garde mémoire passe AVANT la concurrence. Sous
  // pression, la cause affichée doit être la mémoire, pas le nombre de
  // connexions — sinon on cherche au mauvais endroit.
  GE_Refus evaluer() const {
    if (_gardeMemoire && !_g.gardePassee()) return GE_REFUS_GARDE;
    if (_actifs >= _maxConn)                return GE_REFUS_CONCURRENCE;
    return GE_REFUS_AUCUN;
  }

  // ── Arrivée d'une connexion ──────────────────────────────────
  void entrant(ClientType* c) {
    if (c == nullptr) return;

    GE_Refus r = evaluer();
    if (r != GE_REFUS_AUCUN) { refuser(c, r); return; }

    _actifs++;
    if (_actifs > _pic) _pic = _actifs;
    _admis++;
    _refus = GE_REFUS_AUCUN;

    c->setRxTimeout(GE_TCP_RX_TIMEOUT);

    auto req = RequestType::create(&_srv, c);
    if (!req) {
      // Échec d'allocation : le compteur est rendu, le socket est fermé.
      if (_actifs > 0) _actifs--;
      _echecsAlloc++;
      c->abort();
      delete c;
      return;
    }
    // ⚠️ La déconnexion est le SEUL chemin de retour normal du compteur.
    //    Le capturer ici (et pas à la fin de la requête) couvre aussi les
    //    clients qui disparaissent sans réponse.
    req->onDisconnect([this]() { if (_actifs > 0) _actifs--; });
  }

  // ── Refus (ZÉRO allocation) ──────────────────────────────────
  void refuser(ClientType* c, GE_Refus r) {
    _refuses++;
    _refus = r;
    if (r == GE_REFUS_GARDE) _rejGarde++; else _rejConc++;

    // Tampon de PILE : on est exactement dans le cas où le tas est rare.
    // Aucune String, aucun new : ce chemin doit réussir même à 200 o de heap.
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 503 Service Unavailable\r\n"
                     "Retry-After: %lu\r\n"
                     "X-RateLimit-Limit: %u\r\n"
                     "X-Guard-Reason: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n",
                     (unsigned long)((_g.retryAfterMs() + 999UL) / 1000UL),
                     (unsigned)_maxConn,
                     Guard_ESP::refusTexte(r));
    if (n > 0) {
      size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : (sizeof(buf) - 1);
      c->write(buf, len);
    }
    c->close(true);   // IMMÉDIAT puis delete : close(false) = use-after-free
    delete c;
  }

  Guard_ESP& _g;
  ServerType& _srv;
  uint8_t    _maxConn      = GE_TCP_MAX_CONN_DEF;
  uint8_t    _actifs       = 0;
  uint8_t    _pic          = 0;
  uint32_t   _admis        = 0;
  uint32_t   _refuses      = 0;
  uint32_t   _echecsAlloc  = 0;
  uint16_t   _rejGarde     = 0;
  uint16_t   _rejConc      = 0;
  GE_Refus   _refus        = GE_REFUS_AUCUN;
  bool       _gardeMemoire = true;
};

#endif // GUARD_ESP_TCP_H
