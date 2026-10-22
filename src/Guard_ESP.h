// ════════════════════════════════════════════════════════════════
// Guard_ESP — protection anti-OOM / anti-freeze / anti-rafale
// ESP8266 + ESP32 · header-only · ZÉRO allocation dynamique
//
// Rôle : DÉCIDER (admettre, ralentir, rejeter) — à la différence de
// Crash_ESP_Monitor qui OBSERVE (mesure, journalise, diagnostique).
//
// Couvre 7 risques mesurés sur le parc :
//   1. OOM par rafale          → fenêtre glissante PONDÉRÉE (429)
//   2. OOM par réponse grosse  → plafond de taille avant allocation
//   3. Freeze / watchdog       → plafond de concurrence (503) + garde de durée
//   4. Heap bas                → XON/XOFF sur le heap libre
//   5. Fragmentation           → XON/XOFF sur le PLUS GROS BLOC CONTIGU
//   6. Tempête de retries      → backoff + cooldown après reboot
//   7. Démarrage en RAM faible → garde H_ref (refus de servir)
//
// ⚠️ Contraintes de conception (ne pas régresser) :
//   - aucune allocation dynamique : ni malloc, ni new, ni String
//   - tableaux fixes uniquement (dimensionnés par GE_BUCKETS / GE_ROUTES)
//   - admit() répond en O(1), ne bloque jamais, n'appelle jamais delay()
//   - réentrant : appelable depuis loop() et depuis un handler web
//
// Auteur : Olivier FOURNET · Licence : GPL-3.0-only
// ════════════════════════════════════════════════════════════════

#ifndef GUARD_ESP_H
#define GUARD_ESP_H

#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#define GE_VERSION "1.0.0"

// ────────────────────────────────────────────────────────────────
// DIMENSIONNEMENT (compile-time, surchargeable par -D)
// ────────────────────────────────────────────────────────────────
#ifndef GE_BUCKETS
#define GE_BUCKETS 8      // seaux de la fenêtre glissante (1 seau = fenêtre/8)
#endif
#ifndef GE_ROUTES
#define GE_ROUTES 4       // nb de routes suivies par les compteurs (0 = désactivé)
#endif

// ────────────────────────────────────────────────────────────────
// DÉFAUTS GÉNÉRIQUES (écrasés par un profil)
// ────────────────────────────────────────────────────────────────
#define GE_CANAUX_DEF          4
#define GE_FENETRE_DEF         30000UL   // 30 s — valeur éprouvée sur 3 appareils
#define GE_POIDS_MAX_DEF       3
#define GE_SOUS_PRESSION_DEF   1
#define GE_CONCURRENCE_DEF     1
#define GE_DUREE_MAX_DEF       800UL
#define GE_BODY_MAX_DEF        2048UL
#define GE_HEAP_XOFF_DEF       4200UL
#define GE_BLOC_XOFF_DEF       1800UL
#define GE_HEAP_REFUS_DEF      3000UL
#define GE_BLOC_REFUS_DEF      1200UL
#define GE_PART_HEAP_DEF       25        // % du heap max par réponse
#define GE_BACKOFF_BASE_DEF    1000UL
#define GE_BACKOFF_MAX_DEF     30000UL
#define GE_COOLDOWN_REBOOT_DEF 60000UL   // laisse WiFi/NTP se stabiliser
#define GE_INACTIVITE_DEF      300000UL  // 5 min sans activité = suspect

// ────────────────────────────────────────────────────────────────
// PROFILS PAR PLATEFORME
// L'ESP32 a ~15× plus de mémoire contiguë que l'ESP8266 (mesuré :
// 3 442–3 897 o vs 44 843–68 972 o) → profils réellement distincts,
// pas un simple jeu de constantes.
// ────────────────────────────────────────────────────────────────
enum GE_ProfilId : uint8_t {
  GE_PROFIL_AUTO   = 0,   // déduit de la plateforme de compilation
  GE_PROFIL_ESP8266 = 1,  // ressource rare, concurrence interdite
  GE_PROFIL_ESP32   = 2   // ressource abondante, concurrence autorisée
};

// ────────────────────────────────────────────────────────────────
// CAUSES DE REFUS (exposées telles quelles dans les compteurs)
// ────────────────────────────────────────────────────────────────
enum GE_Refus : uint8_t {
  GE_REFUS_AUCUN = 0,
  GE_REFUS_FLUX,          // fenêtre saturée (rafale)
  GE_REFUS_POIDS,         // outil plus lourd que le max effectif (structurel)
  GE_REFUS_PRESSION,      // heap ou bloc contigu sous le seuil (XOFF)
  GE_REFUS_CONCURRENCE,   // trop de requêtes simultanées
  GE_REFUS_BODY,          // taille estimée trop grande pour la mémoire
  GE_REFUS_BACKOFF,       // en cooldown après erreur
  GE_REFUS_GARDE,         // appareil pas encore prêt (H_ref insuffisant)
  GE_REFUS_DUREE          // requête précédente trop longue
};

struct GE_RouteStat {
  const char* nom;
  uint32_t ok;
  uint32_t ko;
};

// ════════════════════════════════════════════════════════════════
// NOYAU
// ════════════════════════════════════════════════════════════════
class Guard_ESP {
public:
  Guard_ESP() { profil(GE_PROFIL_AUTO); }

  // ── Configuration ─────────────────────────────────────────────
  void profil(GE_ProfilId p) {
    if (p == GE_PROFIL_AUTO) {
#if defined(ESP8266)
      p = GE_PROFIL_ESP8266;
#else
      p = GE_PROFIL_ESP32;
#endif
    }
    _profil = p;
    if (p == GE_PROFIL_ESP8266) {
      regler(4, 30000UL, 3, 1, 1, 800UL, 2048UL,
             4200UL, 1800UL, 3000UL, 1200UL, 25);
    } else {
      regler(8, 30000UL, 3, 3, 3, 2500UL, 8192UL,
             25000UL, 12000UL, 20000UL, 10000UL, 10);
    }
    _actif = true;
  }

  void regler(uint16_t canaux, uint32_t fenetreMs, uint16_t poidsMax,
              uint16_t sousPression, uint8_t concurrence, uint32_t dureeMaxMs,
              uint32_t bodyMax, uint32_t heapXoff, uint32_t blocXoff,
              uint32_t heapRefus, uint32_t blocRefus, uint8_t partHeap) {
    _canaux          = canaux ? canaux : 1;
    _fenetreMs       = fenetreMs ? fenetreMs : GE_FENETRE_DEF;
    _poidsMax        = poidsMax ? poidsMax : 1;
    _sousPression    = sousPression ? sousPression : 1;
    _concurrence     = concurrence ? concurrence : 1;
    _dureeMaxMs      = dureeMaxMs;
    _bodyMax         = bodyMax;
    _heapXoff        = heapXoff;
    _blocXoff        = blocXoff;
    _heapRefus       = heapRefus;
    _blocRefus       = blocRefus;
    _partHeap        = partHeap ? partHeap : 10;
    _bucketMs        = _fenetreMs / GE_BUCKETS;
    if (_bucketMs == 0) _bucketMs = 1;
  }

  // Garde de démarrage : refus de servir tant que la marge n'est pas là.
  // Un seuil doit rester SOUS le régime normal, sinon XOFF permanent
  // (incident Telegram 01/09/2026 : garde à 35 Ko au lieu de 30).
  void setGarde(uint32_t heapMin, uint32_t blocMin) {
    _gardeHeap = heapMin;
    _gardeBloc = blocMin;
    _gardeActive = (heapMin || blocMin);
  }

  void setBackoff(uint32_t baseMs, uint32_t maxMs) {
    _backoffBase = baseMs ? baseMs : 1;
    _backoffMax  = maxMs ? maxMs : _backoffBase;
    _backoffCur  = _backoffBase;
  }

  void setInactivite(uint32_t seuilMs) { _inactSeuil = seuilMs; }

  // Auto-calibration depuis H_ref (heap après init WiFi + services).
  //   XOFF      = H_ref × 0.75
  //   refus     = H_ref × 0.55
  //   bloc mini = H_ref × 0.35
  //   canaux    = H_ref > 20000 ? 8 : (H_ref > 10000 ? 6 : 4)
  void autoCalibrer() {
    uint32_t href = heapLibre();
    if (href < 1000) href = 1000;
    uint32_t xoff  = (href * 75) / 100;
    uint32_t refus = (href * 55) / 100;
    uint32_t bloc  = (href * 35) / 100;
    uint16_t can   = (href > 20000UL) ? 8 : ((href > 10000UL) ? 6 : 4);
    uint8_t  conc  = (href > 20000UL) ? 3 : 1;
    setGarde(refus, (bloc * 2) / 5);
    regler(can, GE_FENETRE_DEF, 3, (conc > 1 ? 3 : 1), conc,
           _dureeMaxMs, _bodyMax, xoff, bloc, refus, (bloc * 2) / 5, _partHeap);
  }

  void demarrer() {
    _derniereActivite = millis();
    _cooldownFin = _derniereActivite + GE_COOLDOWN_REBOOT_DEF;
    _winStart = 0;
    for (uint8_t i = 0; i < GE_BUCKETS; i++) _bucket[i] = 0;
    _idx = 0;
  }

  // ── Mesure mémoire (déléguée à Crash_ESP_Monitor si présent) ──
  static uint32_t heapLibre() {
#if defined(ESP8266)
    return (uint32_t)ESP.getFreeHeap();
#else
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
#endif
  }

  static uint32_t blocLibre() {
#if defined(ESP8266)
    return (uint32_t)ESP.getMaxFreeBlockSize();
#else
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
#endif
  }

  bool enPression() const {
    return (heapLibre() < _heapXoff) || (blocLibre() < _blocXoff);
  }

  bool gardePassee() const {
    if (!_gardeActive) return true;
    return (heapLibre() >= _gardeHeap) && (blocLibre() >= _gardeBloc);
  }

  // ── Décision principale ───────────────────────────────────────
  // poids  : coût de la requête (1 léger … 3 lourd)
  // taille : taille estimée de la réponse en octets (0 = inconnue)
  // Retourne true = la requête peut être traitée.
  bool admit(uint8_t poids = 1, uint32_t taille = 0) {
    if (!_actif) { _admis++; return true; }
    if (poids == 0) poids = 1;
    uint32_t now = millis();

    // 1. Cooldown post-reboot / post-erreur
    if (_cooldownFin && (int32_t)(now - _cooldownFin) < 0) {
      return refuser(GE_REFUS_BACKOFF);
    }

    // 2. Garde de démarrage
    if (!gardePassee()) return refuser(GE_REFUS_GARDE);

    // 3. Pression mémoire (XON/XOFF)
    bool xoff = enPression();
    uint16_t maxEff = xoff ? _sousPression : _canaux;

    // 4. Outil structurellement trop lourd pour l'état de flux
    //    (message distinct du « saturé » : sinon diagnostic trompeur,
    //     cf. Server_MCP v1.3.1 « canaux 0/2 »)
    if (poids > maxEff) {
      _xoff = xoff;
      return refuser(xoff ? GE_REFUS_PRESSION : GE_REFUS_POIDS);
    }

    // 5. Concurrence
    if (_actifs >= _concurrence) return refuser(GE_REFUS_CONCURRENCE);

    // 6. Taille de réponse : plafond absolu puis part du heap
    if (taille) {
      if (taille > _bodyMax) return refuser(GE_REFUS_BODY);
      uint32_t part = (heapLibre() * _partHeap) / 100;
      if (taille > part) return refuser(GE_REFUS_BODY);
    }

    // 7. Fenêtre glissante pondérée
    avancer(now);
    if ((uint32_t)(charge() + poids) > maxEff) {
      _xoff = xoff;
      return refuser(GE_REFUS_FLUX);
    }

    // Admis
    _bucket[_idx] = (uint16_t)(_bucket[_idx] + poids);
    _actifs++;
    _reqStart = now;
    _admis++;
    _xoff = xoff;
    _derniereActivite = now;
    _refus = GE_REFUS_AUCUN;
    return true;
  }

  // Libère le slot de concurrence et mesure la durée réelle.
  void sortir() {
    if (_actifs) _actifs--;
    uint32_t d = millis() - _reqStart;
    if (d > _dureePire) _dureePire = d;
    if (_dureeMaxMs && d > _dureeMaxMs) {
      _depassements++;
      _cooldownFin = millis() + _backoffCur;
      _backoffCur = (_backoffCur * 2 > _backoffMax) ? _backoffMax : _backoffCur * 2;
    } else {
      _backoffCur = _backoffBase;
    }
  }

  // ── Watchdog d'inactivité ─────────────────────────────────────
  // Comble un angle mort réel : une poussée Domoticz arrêtée 9 h 34
  // sans que rien ne le signale (Coulometre F-19).
  void tick() {
    uint32_t now = millis();
    if (_actifs && _dureeMaxMs && (uint32_t)(now - _reqStart) > _dureeMaxMs * 4) {
      _actifs = 0;
      _depassements++;
    }
  }

  void activite() { _derniereActivite = millis(); }

  uint32_t inactiviteMs() const { return millis() - _derniereActivite; }

  bool inactif() const { return _inactSeuil && (inactiviteMs() > _inactSeuil); }

  // ── Compteurs par route ───────────────────────────────────────
  void route(const char* nom) {
#if GE_ROUTES > 0
    _routeCur = -1;
    for (uint8_t i = 0; i < GE_ROUTES; i++) {
      if (_routes[i].nom == nom || (_routes[i].nom && nom && strcmp(_routes[i].nom, nom) == 0)) {
        _routeCur = (int8_t)i;
        return;
      }
      if (!_routes[i].nom) {
        _routes[i].nom = nom;
        _routeCur = (int8_t)i;
        return;
      }
    }
#else
    (void)nom;
#endif
  }

  void routeOk() {
#if GE_ROUTES > 0
    if (_routeCur >= 0) _routes[(uint8_t)_routeCur].ok++;
#endif
  }

  void routeKo() {
#if GE_ROUTES > 0
    if (_routeCur >= 0) _routes[(uint8_t)_routeCur].ko++;
#endif
  }

  const GE_RouteStat* routes() const { return _routes; }

  // ── Accès / diagnostic ────────────────────────────────────────
  GE_Refus refus() const { return _refus; }

  static const char* refusTexte(GE_Refus r) {
    switch (r) {
      case GE_REFUS_FLUX:         return "flux";
      case GE_REFUS_POIDS:        return "poids";
      case GE_REFUS_PRESSION:     return "pression";
      case GE_REFUS_CONCURRENCE:  return "concurrence";
      case GE_REFUS_BODY:         return "body";
      case GE_REFUS_BACKOFF:      return "backoff";
      case GE_REFUS_GARDE:        return "garde";
      case GE_REFUS_DUREE:        return "duree";
      default:                    return "aucun";
    }
  }

  // Délai à annoncer au client (en-tête Retry-After, en secondes).
  uint32_t retryAfterMs() const {
    if (_refus == GE_REFUS_BACKOFF) {
      uint32_t now = millis();
      return ((int32_t)(_cooldownFin - now) > 0) ? (_cooldownFin - now) : 1000;
    }
    return _fenetreMs;
  }

  uint16_t canaux() const { return _canaux; }
  uint32_t fenetreMs() const { return _fenetreMs; }
  uint16_t maxSousPression() const { return _sousPression; }
  uint16_t chargeCourante() { avancer(millis()); return charge(); }
  bool xoff() const { return _xoff; }
  bool actif() const { return _actif; }
  void actif(bool v) { _actif = v; }
  uint8_t actifs() const { return _actifs; }
  uint8_t concurrence() const { return _concurrence; }
  uint32_t bodyMax() const { return _bodyMax; }
  uint32_t dureeMaxMs() const { return _dureeMaxMs; }
  GE_ProfilId profilId() const { return _profil; }

  const char* profilTexte() const {
    return (_profil == GE_PROFIL_ESP8266) ? "ESP8266" : "ESP32";
  }

  uint32_t admis() const { return _admis; }
  uint32_t rejetes() const { return _rejetes; }
  uint32_t rejets(GE_Refus r) const {
    switch (r) {
      case GE_REFUS_FLUX:        return _rejFlux;
      case GE_REFUS_POIDS:       return _rejPoids;
      case GE_REFUS_PRESSION:    return _rejPression;
      case GE_REFUS_CONCURRENCE: return _rejConcurrence;
      case GE_REFUS_BODY:        return _rejBody;
      case GE_REFUS_BACKOFF:     return _rejBackoff;
      case GE_REFUS_GARDE:       return _rejGarde;
      case GE_REFUS_DUREE:       return _rejDuree;
      default:                   return 0;
    }
  }
  uint32_t dureePire() const { return _dureePire; }
  uint32_t depassements() const { return _depassements; }

  // Résumé texte SANS String (écrit dans un buffer appelant).
  // Retourne le nombre de caractères écrits (hors NUL).
  size_t resume(char* buf, size_t len) const {
    if (!buf || !len) return 0;
    int n = snprintf(buf, len,
                     "Guard_ESP %s | profil %s %u/%lus | charge %u/%u%s | actifs %u/%u"
                     " | admis %lu rejets %lu (flux %lu poids %lu pression %lu conc %lu body %lu backoff %lu garde %lu)"
                     " | heap %lu bloc %lu | duree pire %lums depass %lu",
                     GE_VERSION, profilTexte(),
                     (unsigned)_canaux, (unsigned long)(_fenetreMs / 1000),
                     (unsigned)_bucket[_idx], (unsigned)(_xoff ? _sousPression : _canaux),
                     _xoff ? " XOFF" : "",
                     (unsigned)_actifs, (unsigned)_concurrence,
                     (unsigned long)_admis, (unsigned long)_rejetes,
                     (unsigned long)_rejFlux, (unsigned long)_rejPoids,
                     (unsigned long)_rejPression, (unsigned long)_rejConcurrence,
                     (unsigned long)_rejBody, (unsigned long)_rejBackoff,
                     (unsigned long)_rejGarde,
                     (unsigned long)heapLibre(), (unsigned long)blocLibre(),
                     (unsigned long)_dureePire, (unsigned long)_depassements);
    return (n > 0) ? (size_t)n : 0;
  }

private:
  // ── Fenêtre glissante par seaux (O(1), sans allocation) ───────
  void avancer(uint32_t now) {
    if (_winStart == 0) { _winStart = now; return; }
    uint8_t garde = GE_BUCKETS;
    while ((uint32_t)(now - _winStart) >= _bucketMs && garde--) {
      _winStart += _bucketMs;
      _idx = (uint8_t)((_idx + 1) % GE_BUCKETS);
      _bucket[_idx] = 0;
    }
    // Trou > fenêtre entière : tout est périmé.
    if ((uint32_t)(now - _winStart) >= _fenetreMs) {
      for (uint8_t i = 0; i < GE_BUCKETS; i++) _bucket[i] = 0;
      _winStart = now;
    }
  }

  uint16_t charge() const {
    uint32_t s = 0;
    for (uint8_t i = 0; i < GE_BUCKETS; i++) s += _bucket[i];
    return (s > 0xFFFF) ? 0xFFFF : (uint16_t)s;
  }

  bool refuser(GE_Refus r) {
    _refus = r;
    _rejetes++;
    switch (r) {
      case GE_REFUS_FLUX:        _rejFlux++;        break;
      case GE_REFUS_POIDS:       _rejPoids++;       break;
      case GE_REFUS_PRESSION:    _rejPression++;    break;
      case GE_REFUS_CONCURRENCE: _rejConcurrence++; break;
      case GE_REFUS_BODY:        _rejBody++;        break;
      case GE_REFUS_BACKOFF:     _rejBackoff++;     break;
      case GE_REFUS_GARDE:       _rejGarde++;       break;
      case GE_REFUS_DUREE:       _rejDuree++;       break;
      default: break;
    }
    return false;
  }

  // ── Configuration active ──────────────────────────────────────
  uint16_t _canaux       = GE_CANAUX_DEF;
  uint16_t _poidsMax     = GE_POIDS_MAX_DEF;
  uint16_t _sousPression = GE_SOUS_PRESSION_DEF;
  uint32_t _fenetreMs    = GE_FENETRE_DEF;
  uint32_t _bucketMs     = GE_FENETRE_DEF / GE_BUCKETS;
  uint32_t _heapXoff     = GE_HEAP_XOFF_DEF;
  uint32_t _blocXoff     = GE_BLOC_XOFF_DEF;
  uint32_t _heapRefus    = GE_HEAP_REFUS_DEF;
  uint32_t _blocRefus    = GE_BLOC_REFUS_DEF;
  uint32_t _bodyMax      = GE_BODY_MAX_DEF;
  uint32_t _dureeMaxMs   = GE_DUREE_MAX_DEF;
  uint32_t _gardeHeap    = 0;
  uint32_t _gardeBloc    = 0;
  uint32_t _inactSeuil   = GE_INACTIVITE_DEF;
  uint8_t  _concurrence  = GE_CONCURRENCE_DEF;
  uint8_t  _partHeap     = GE_PART_HEAP_DEF;
  bool     _gardeActive  = false;

  // ── Fenêtre ───────────────────────────────────────────────────
  uint16_t _bucket[GE_BUCKETS];
  uint32_t _winStart = 0;
  uint8_t  _idx      = 0;

  // ── Concurrence / durée ───────────────────────────────────────
  uint8_t  _actifs   = 0;
  uint32_t _reqStart = 0;

  // ── Backoff ───────────────────────────────────────────────────
  uint32_t _backoffBase = GE_BACKOFF_BASE_DEF;
  uint32_t _backoffMax  = GE_BACKOFF_MAX_DEF;
  uint32_t _backoffCur  = GE_BACKOFF_BASE_DEF;
  uint32_t _cooldownFin = 0;

  // ── Inactivité ────────────────────────────────────────────────
  uint32_t _derniereActivite = 0;

  // ── Compteurs ─────────────────────────────────────────────────
  uint32_t _admis         = 0;
  uint32_t _rejetes       = 0;
  uint32_t _rejFlux       = 0;
  uint32_t _rejPoids      = 0;
  uint32_t _rejPression   = 0;
  uint32_t _rejConcurrence= 0;
  uint32_t _rejBody       = 0;
  uint32_t _rejBackoff    = 0;
  uint32_t _rejGarde      = 0;
  uint32_t _rejDuree      = 0;
  uint32_t _dureePire     = 0;
  uint32_t _depassements  = 0;

  // ── État ──────────────────────────────────────────────────────
  GE_Refus   _refus    = GE_REFUS_AUCUN;
  GE_ProfilId _profil  = GE_PROFIL_AUTO;
  bool       _actif    = true;
  bool       _xoff     = false;

  // ── Routes ────────────────────────────────────────────────────
  GE_RouteStat _routes[GE_ROUTES > 0 ? GE_ROUTES : 1] = {};
  int8_t _routeCur = -1;
};

// Cohérence de réglage.
// NOTE : sousPression < poidsMax est LÉGITIME — sous pression mémoire, les
// requêtes lourdes doivent être refusées. Le bug historique (Server_MCP
// v1.3.1 « canaux 0/2 ») venait d'un message trompeur et d'un XOFF
// permanent, pas de la valeur elle-même. D'où : sous pression on renvoie
// GE_REFUS_PRESSION, hors pression GE_REFUS_POIDS (cause structurelle).
static_assert(GE_SOUS_PRESSION_DEF >= 1, "GE_SOUS_PRESSION_DEF doit etre >= 1");
static_assert(GE_POIDS_MAX_DEF >= 1,     "GE_POIDS_MAX_DEF doit etre >= 1");
static_assert(GE_BUCKETS >= 2,           "GE_BUCKETS doit etre >= 2");
static_assert(GE_CONCURRENCE_DEF >= 1,   "GE_CONCURRENCE_DEF doit etre >= 1");

// Instance globale prête à l'emploi (une seule par firmware).
inline Guard_ESP& guardInstance() {
  static Guard_ESP g;
  return g;
}

#endif // GUARD_ESP_H
