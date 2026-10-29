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
// ⚠️ CONTRAINTES DE CONCEPTION (ne pas régresser)
//   - aucune allocation dynamique dans CE fichier : ni malloc, ni new, ni String
//   - tableaux fixes uniquement (GE_BUCKETS / GE_ROUTES / GE_SLOTS)
//   - admit() répond en O(1), ne bloque jamais, n'appelle jamais delay()
//
// ⚠️ THREAD-SAFETY
//   Les MUTATIONS d'état (admitInterne, sortir, tick, route*, viderFenetre)
//   sont protégées par un verrou critique (portMUX sur ESP32, sans objet sur
//   ESP8266 mono-cœur). Les ACCESSEURS DE LECTURE ne sont pas verrouillés :
//   ne pas les appeler depuis une autre tâche en croyant obtenir une vue
//   atomique. Les accesseurs MUTANTS (chargeCourante, creditRestant, resume)
//   font vieillir la fenêtre : les réserver à la tâche propriétaire.
//
// Auteur : Olivier FOURNET · Licence : GPL-3.0-only
// ════════════════════════════════════════════════════════════════

#ifndef GUARD_ESP_H
#define GUARD_ESP_H

#include <Arduino.h>
#include <string.h>
#include <stdio.h>

// Version de la librairie (source UNIQUE — agrégée par liste_lib.h des projets)
#define __GUARD_ESP_H_VERSION__ "1.2.0"
#define GE_VERSION __GUARD_ESP_H_VERSION__

// Signale à l'appelant qu'ignorer le résultat d'admit() est un bug :
// un ticket détruit immédiatement rend le slot AVANT le traitement.
#if defined(__GNUC__)
  #define GE_NODISCARD __attribute__((warn_unused_result))
#else
  #define GE_NODISCARD
#endif

// ────────────────────────────────────────────────────────────────
// DIMENSIONNEMENT (compile-time, surchargeable par -D)
// ────────────────────────────────────────────────────────────────
#ifndef GE_BUCKETS
#define GE_BUCKETS 8      // seaux PLEINS de la fenêtre (1 seau = fenêtre/GE_BUCKETS)
#endif
#ifndef GE_ROUTES
#define GE_ROUTES 4       // routes suivies par les compteurs (0 = désactivé)
#endif
#ifndef GE_SLOTS
#define GE_SLOTS 4        // requêtes simultanées possibles (borne haute de concurrence)
#endif
#ifndef GE_ROUTE_NOM_MAX
#define GE_ROUTE_NOM_MAX 0   // 0 = pointeur (littéral) ; >0 = copie du nom (coût RAM)
#endif

// Bornes hautes : sans elles, un `for (uint8_t i…)` boucle à l'infini et un
// `int8_t` d'index devient négatif. Vérifié : GE_BUCKETS=256 bloquait demarrer().
static_assert(GE_BUCKETS >= 2 && GE_BUCKETS <= 64, "GE_BUCKETS doit etre dans [2,64]");
static_assert(GE_ROUTES <= 64,                      "GE_ROUTES doit etre <= 64");
static_assert(GE_SLOTS >= 1 && GE_SLOTS <= 13,      "GE_SLOTS doit etre dans [1,13]");
static_assert(GE_ROUTE_NOM_MAX == 0 || GE_ROUTE_NOM_MAX >= 4,
              "GE_ROUTE_NOM_MAX doit valoir 0 ou >= 4");

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
  GE_PROFIL_AUTO    = 0,   // déduit de la plateforme de compilation
  GE_PROFIL_ESP8266 = 1,   // ressource rare, concurrence interdite
  GE_PROFIL_ESP32   = 2    // ressource abondante, concurrence autorisée
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
  GE_REFUS_GARDE          // appareil pas encore prêt (H_ref insuffisant)
};

struct GE_RouteStat {
  const char* nom;
  uint32_t ok;
  uint32_t ko;
};

class Guard_ESP;

// ────────────────────────────────────────────────────────────────
// Verrou critique : protège les mutations d'état du noyau.
// ESP8266 = mono-cœur, aucun parallélisme sur ces appels → sans objet.
// ESP32   = deux cœurs + tâche async_tcp → portMUX indispensable.
// ────────────────────────────────────────────────────────────────
#if defined(ESP32)
  #define GE_VERROU(m)   portENTER_CRITICAL(m)
  #define GE_DEVERROU(m) portEXIT_CRITICAL(m)
#else
  #define GE_VERROU(m)
  #define GE_DEVERROU(m)
#endif

// ════════════════════════════════════════════════════════════════
// TICKET — jeton RAII d'une requête admise
// Remplace l'ancien couple admit()/fin() des adaptateurs, qui stockait
// l'état « ouvert » dans un booléen PAR INSTANCE : deux requêtes
// simultanées écrasaient ce booléen et un slot de concurrence était
// perdu définitivement (mesuré : actifs=1 après 2 admits + 2 fins).
// Un ticket porte SON index de slot : la libération est exacte.
// ════════════════════════════════════════════════════════════════
class GE_Ticket {
public:
  GE_Ticket() : _g(nullptr), _idx(GE_SLOT_AUCUN) {}
  GE_Ticket(Guard_ESP& g, bool ok, uint8_t idx)
    : _g(ok ? &g : nullptr), _idx(ok ? idx : GE_SLOT_AUCUN) {}

  ~GE_Ticket() { liberer(); }

  GE_Ticket(GE_Ticket&& o) noexcept : _g(o._g), _idx(o._idx) {
    o._g = nullptr; o._idx = GE_SLOT_AUCUN;
  }
  GE_Ticket& operator=(GE_Ticket&& o) noexcept {
    if (this != &o) { liberer(); _g = o._g; _idx = o._idx; o._g = nullptr; o._idx = GE_SLOT_AUCUN; }
    return *this;
  }
  GE_Ticket(const GE_Ticket&) = delete;
  GE_Ticket& operator=(const GE_Ticket&) = delete;

  explicit operator bool() const { return _idx != GE_SLOT_AUCUN; }
  bool ok() const { return _idx != GE_SLOT_AUCUN; }

private:
  static const uint8_t GE_SLOT_AUCUN = 0xFF;
  void liberer();
  Guard_ESP* _g;
  uint8_t    _idx;
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
      regler(4, 30000UL, 3, 1, borneConcurrence(1), 800UL, 2048UL,
             4200UL, 1800UL, 3000UL, 1200UL, 25);
    } else {
      regler(8, 30000UL, 3, 3, borneConcurrence(3), 2500UL, 8192UL,
             25000UL, 12000UL, 20000UL, 10000UL, 10);
    }
    // Le profil ARME la garde de démarrage avec les seuils « refus total ».
    // Sans cela, _heapRefus/_blocRefus étaient écrits mais jamais lus
    // (finding W10) et la garde restait inactive par défaut.
    setGarde(_heapRefus, _blocRefus);
    _actif = true;
  }

  // Retourne false si un paramètre est hors bornes : la configuration
  // précédente est alors CONSERVÉE (aucune écriture partielle).
  bool regler(uint16_t canaux, uint32_t fenetreMs, uint16_t poidsMax,
              uint16_t sousPression, uint8_t concurrence, uint32_t dureeMaxMs,
              uint32_t bodyMax, uint32_t heapXoff, uint32_t blocXoff,
              uint32_t heapRefus, uint32_t blocRefus, uint8_t partHeap) {
    if (canaux == 0 || canaux > 4096)                 return false;
    if (fenetreMs < 1000UL || fenetreMs > 3600000UL)  return false;
    if (poidsMax == 0)                                return false;
    if (sousPression == 0)                            return false;
    if (concurrence == 0 || concurrence > GE_SLOTS)   return false;
    if (dureeMaxMs > 600000UL)                        return false;   // borne anti-débordement (×4)
    if (bodyMax == 0)                                 return false;
    if (partHeap == 0 || partHeap > 100)              return false;
    if (heapRefus > heapXoff)                         return false;
    if (blocRefus > blocXoff)                         return false;

    _canaux          = canaux;
    _fenetreMs       = fenetreMs;
    _poidsMax        = poidsMax;
    _sousPression    = sousPression;
    _concurrence     = concurrence;
    _dureeMaxMs      = dureeMaxMs;
    _bodyMax         = bodyMax;
    _heapXoff        = heapXoff;
    _blocXoff        = blocXoff;
    _heapRefus       = heapRefus;
    _blocRefus       = blocRefus;
    _partHeap        = partHeap;
    _bucketMs        = _fenetreMs / GE_BUCKETS;
    if (_bucketMs == 0) _bucketMs = 1;
    // La granularité change : les comptes acquis avec l'ancienne doivent
    // être purgés, sinon le quota effectif devient incohérent (S2).
    GE_VERROU(&_mux);
    viderFenetre(millis());
    GE_DEVERROU(&_mux);
    return true;
  }

  // Garde de démarrage : refus de servir tant que la marge n'est pas là.
  // Un seuil doit rester SOUS le régime normal, sinon XOFF permanent
  // (incident Telegram 01/09/2026 : garde à 35 Ko au lieu de 30).
  void setGarde(uint32_t heapMin, uint32_t blocMin) {
    _gardeHeap   = heapMin;
    _gardeBloc   = blocMin;
    _gardeActive = (heapMin || blocMin);
  }

  void setBackoff(uint32_t baseMs, uint32_t maxMs) {
    if (baseMs == 0) baseMs = 1;
    if (maxMs < baseMs) maxMs = baseMs;
    _backoffBase = baseMs;
    _backoffMax  = maxMs;
    _backoffCur  = baseMs;
  }

  void setInactivite(uint32_t seuilMs) { _inactSeuil = seuilMs; }

  // Auto-calibration depuis H_ref (heap après init WiFi + services).
  // Formules (identiques au LIVRE_BLANC §3.5) :
  //   heapXoff   = 0,75 × H_ref      blocXoff = 0,35 × H_ref
  //   gardeHeap  = 0,55 × H_ref      gardeBloc = 0,25 × H_ref
  //   canaux     = H_ref > 20000 ? 8 : (H_ref > 10000 ? 6 : 4)
  //   concurrence= H_ref > 20000 ? 3 : 1
  // ⚠️ Écrase la configuration courante : appeler AVANT setGarde().
  void autoCalibrer() {
    uint32_t href = heapLibre();
    if (href < 1000UL) href = 1000UL;
    uint32_t heapXoff  = (href * 75UL) / 100UL;
    uint32_t blocXoff  = (href * 35UL) / 100UL;
    uint32_t gardeHeap = (href * 55UL) / 100UL;
    uint32_t gardeBloc = (href * 25UL) / 100UL;
    uint16_t can   = (href > 20000UL) ? 8 : ((href > 10000UL) ? 6 : 4);
    uint8_t  conc  = borneConcurrence((href > 20000UL) ? 3 : 1);
    uint16_t sousP = (conc > 1) ? 3 : 1;
    // regler() peut refuser (paramètre hors bornes) : on n'arme alors PAS la
    // garde, sinon elle serait incohérente avec un XOFF non appliqué — c'est
    // exactement le motif de l'incident du 01/09 (garde > XOFF).
    if (regler(can, GE_FENETRE_DEF, 3, sousP, conc,
               _dureeMaxMs, _bodyMax, heapXoff, blocXoff, gardeHeap, gardeBloc, _partHeap)) {
      setGarde(gardeHeap, gardeBloc);
    }
  }

  // Arme le cooldown post-reboot et remet la fenêtre à zéro.
  void demarrer() {
    GE_VERROU(&_mux);
    _derniereActivite = millis();
    _cooldownFin = _derniereActivite + GE_COOLDOWN_REBOOT_DEF;
    viderFenetre(_derniereActivite);
    _refus = GE_REFUS_AUCUN;
    GE_DEVERROU(&_mux);
  }

  // ── Mesure mémoire (hors verrou : ne pas couper les interruptions) ──
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

  static bool enPression(uint32_t heap, uint32_t bloc, uint32_t seuilHeap, uint32_t seuilBloc) {
    return (heap < seuilHeap) || (bloc < seuilBloc);
  }

  bool enPression() const { return enPression(heapLibre(), blocLibre(), _heapXoff, _blocXoff); }

  bool gardePassee() const {
    if (!_gardeActive) return true;
    return (heapLibre() >= _gardeHeap) && (blocLibre() >= _gardeBloc);
  }

  // ── Décision principale ───────────────────────────────────────
  // poids  : coût de la requête (1 léger … _poidsMax lourd)
  // taille : taille du PLUS GROS BLOC alloué pour servir la requête
  //          (pas la taille totale du corps : une réponse streamée par
  //          blocs de 192 o passe avec taille = 192)
  // Retourne true = slot acquis, à rendre par sortir().
  // ⚠️ Le résultat ne doit pas être ignoré : utiliser un GE_Ticket.
  GE_NODISCARD bool admit(uint8_t poids = 1, uint32_t taille = 0) {
    uint8_t idx = GE_SLOT_AUCUN;
    bool ok = admitInterne(poids, taille, idx);
    if (ok) {
      GE_VERROU(&_mux);
      _slotDernier = idx;
      GE_DEVERROU(&_mux);
    }
    return ok;
  }

  // Variante ticket : libération automatique et exacte (RAII).
  // ⚠️ Le ticket doit être conservé dans une variable de portée handler.
  GE_NODISCARD GE_Ticket ticket(uint8_t poids = 1, uint32_t taille = 0) {
    uint8_t idx = GE_SLOT_AUCUN;
    bool ok = admitInterne(poids, taille, idx);
    if (ok) {
      GE_VERROU(&_mux);
      _slotDernier = idx;
      GE_DEVERROU(&_mux);
    }
    return GE_Ticket(*this, ok, idx);
  }

  // Rend le slot acquis par le dernier admit() (usage MONO-REQUÊTE).
  // Avec _concurrence > 1, préférer le GE_Ticket : _slotDernier ne désigne
  // que le dernier slot ouvert.
  void sortir() {
    uint8_t idx;
    GE_VERROU(&_mux);
    idx = _slotDernier;
    GE_DEVERROU(&_mux);
    sortir(idx);
  }

  // Rend un slot précis (usage concurrent / GE_Ticket).
  void sortir(uint8_t idx) {
    GE_VERROU(&_mux);
    // sortir() sans admit(), ou slot déjà rendu par tick() : ne rien mesurer.
    if (idx >= GE_SLOTS || !_slotBusy[idx]) {
      GE_DEVERROU(&_mux);
      return;
    }
    bool fictif = _slotFictif[idx];
    _slotBusy[idx]    = false;
    _slotFictif[idx]  = false;
    if (_actifs) _actifs--;
    GE_DEVERROU(&_mux);

    // Slot fictif (protection désactivée) : aucun temps à mesurer, sinon
    // la durée serait aberrante (finding C4 : dureePire = 105 000 ms).
    if (fictif) return;

    uint32_t d = millis() - _slotStart[idx];
    if (d > _dureePire) _dureePire = d;
    if (_dureeMaxMs && d > _dureeMaxMs) {
      _depassements++;
      _cooldownFin = millis() + _backoffCur;
      uint32_t suivant = _backoffCur * 2UL;
      if (suivant < _backoffCur) suivant = _backoffMax;      // anti-débordement
      _backoffCur = (suivant > _backoffMax) ? _backoffMax : suivant;
    } else {
      _backoffCur = _backoffBase;
    }
  }

  // ── Watchdog de concurrence ───────────────────────────────────
  // Libère les slots restés ouverts anormalement longtemps (handler qui
  // n'a jamais rendu la main). Les slots sont marqués libres, donc le
  // sortir() ultérieur ne recompte pas (finding W3), et _actifs est
  // DÉCRÉMENTÉ par slot réellement libéré — le remettre à 0 laissait
  // passer plus de requêtes que _concurrence (finding 30).
  void tick() {
    if (_dureeMaxMs == 0) return;
    uint32_t limite = _dureeMaxMs * 4UL;
    uint32_t now = millis();
    uint8_t liberes = 0;
    GE_VERROU(&_mux);
    for (uint8_t i = 0; i < GE_SLOTS; i++) {
      if (_slotBusy[i] && (uint32_t)(now - _slotStart[i]) > limite) {
        _slotBusy[i]   = false;
        _slotFictif[i] = false;
        liberes++;
      }
    }
    if (liberes) {
      _actifs = (_actifs > liberes) ? (uint8_t)(_actifs - liberes) : 0;
      _depassements++;
    }
    GE_DEVERROU(&_mux);
  }

  void activite() { _derniereActivite = millis(); }

  uint32_t inactiviteMs() const { return millis() - _derniereActivite; }

  bool inactif() const { return _inactSeuil && (inactiviteMs() > _inactSeuil); }

  // ── Compteurs par route ───────────────────────────────────────
  // ⚠️ `nom` doit désigner une chaîne de durée de vie >= celle du programme
  // (littéral ou buffer statique). La comparaison se fait PAR CHAÎNE : deux
  // littéraux distincts de même contenu désignent la même route.
  void route(const char* nom) {
#if GE_ROUTES > 0
    GE_VERROU(&_mux);
    if (!nom) { _routeCur = -1; GE_DEVERROU(&_mux); return; }
    for (uint8_t i = 0; i < GE_ROUTES; i++) {
      if (!_routes[i].nom) {
#if GE_ROUTE_NOM_MAX > 0
        strncpy(_routeNoms[i], nom, GE_ROUTE_NOM_MAX - 1);
        _routeNoms[i][GE_ROUTE_NOM_MAX - 1] = '\0';
        _routes[i].nom = _routeNoms[i];
#else
        _routes[i].nom = nom;
#endif
        _routeCur = (int8_t)i;
        GE_DEVERROU(&_mux);
        return;
      }
      if (strcmp(_routes[i].nom, nom) == 0) {
        _routeCur = (int8_t)i;
        GE_DEVERROU(&_mux);
        return;
      }
    }
    _routeCur = -1;
    _routesSaturees = true;     // table pleine : signalé, plus silencieux
    GE_DEVERROU(&_mux);
#else
    (void)nom;
#endif
  }

  void routeOk() {
#if GE_ROUTES > 0
    GE_VERROU(&_mux);
    if (_routeCur >= 0) _routes[(uint8_t)_routeCur].ok++;
    GE_DEVERROU(&_mux);
#endif
  }

  void routeKo() {
#if GE_ROUTES > 0
    GE_VERROU(&_mux);
    if (_routeCur >= 0) _routes[(uint8_t)_routeCur].ko++;
    GE_DEVERROU(&_mux);
#endif
  }

  const GE_RouteStat* routes() const { return _routes; }
  bool routesSaturees() const { return _routesSaturees; }

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
      default:                    return "aucun";
    }
  }

  // Délai à annoncer au client (en-tête Retry-After, en secondes).
  // Un événement reste compté jusqu'à GE_RING × _bucketMs (anneau de
  // GE_BUCKETS+1 seaux) : annoncer la fenêtre seule ferait refuser une
  // seconde fois le client qui respecte l'en-tête (finding 39).
  uint32_t retryAfterMs() const {
    if (_refus == GE_REFUS_BACKOFF) {
      uint32_t now = millis();
      return ((int32_t)(_cooldownFin - now) > 0) ? (_cooldownFin - now) : 1000UL;
    }
    if (_refus == GE_REFUS_CONCURRENCE) return 1000UL;   // un slot se libère vite
    return (uint32_t)GE_RING * _bucketMs;
  }

  uint16_t canaux() const { return _canaux; }
  uint16_t poidsMax() const { return _poidsMax; }
  uint32_t fenetreMs() const { return _fenetreMs; }
  uint16_t maxSousPression() const { return _sousPression; }

  // Accesseurs MUTANTS : ils font vieillir la fenêtre. À réserver à la
  // tâche propriétaire du garde (ne pas appeler depuis un autre cœur).
  uint16_t chargeCourante() {
    GE_VERROU(&_mux);
    avancer(millis());
    uint16_t c = charge();
    GE_DEVERROU(&_mux);
    return c;
  }

  uint16_t creditRestant() {
    GE_VERROU(&_mux);
    avancer(millis());
    uint16_t maxEff = _xoff ? _sousPression : _canaux;
    uint16_t c = charge();
    GE_DEVERROU(&_mux);
    return (c >= maxEff) ? 0 : (uint16_t)(maxEff - c);
  }

  bool xoff() const { return _xoff; }
  bool actif() const { return _actif; }
  void actif(bool v) { _actif = v; }
  uint8_t actifs() const { return _actifs; }
  uint8_t concurrence() const { return _concurrence; }
  uint32_t bodyMax() const { return _bodyMax; }
  uint32_t dureeMaxMs() const { return _dureeMaxMs; }
  uint32_t heapXoff() const { return _heapXoff; }
  uint32_t blocXoff() const { return _blocXoff; }
  uint32_t heapRefus() const { return _heapRefus; }
  uint32_t blocRefus() const { return _blocRefus; }
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
      default:                   return 0;
    }
  }
  uint32_t dureePire() const { return _dureePire; }
  uint32_t depassements() const { return _depassements; }

  // Résumé texte SANS String. Retourne le nombre de caractères RÉELLEMENT
  // écrits (hors NUL), donc <= len-1.
  // MUTANT : fait vieillir la fenêtre pour afficher la charge réelle
  // (sinon « charge 4/4 » restait affiché après 10 min d'inactivité).
  size_t resume(char* buf, size_t len) {
    if (!buf || len < 2) return 0;
    uint16_t ch;
    uint16_t maxEff;
    GE_VERROU(&_mux);
    avancer(millis());
    ch = charge();
    maxEff = _xoff ? _sousPression : _canaux;
    GE_DEVERROU(&_mux);

    int n = snprintf(buf, len,
                     "Guard_ESP %s | profil %s %u/%lus | charge %u/%u%s | actifs %u/%u"
                     " | admis %lu rejets %lu (flux %lu poids %lu pression %lu conc %lu"
                     " body %lu backoff %lu garde %lu)"
                     " | heap %lu bloc %lu | duree pire %lums depass %lu",
                     GE_VERSION, profilTexte(),
                     (unsigned)_canaux, (unsigned long)(_fenetreMs / 1000UL),
                     (unsigned)ch, (unsigned)maxEff,
                     _xoff ? " XOFF" : "",
                     (unsigned)_actifs, (unsigned)_concurrence,
                     (unsigned long)_admis, (unsigned long)_rejetes,
                     (unsigned long)_rejFlux, (unsigned long)_rejPoids,
                     (unsigned long)_rejPression, (unsigned long)_rejConcurrence,
                     (unsigned long)_rejBody, (unsigned long)_rejBackoff,
                     (unsigned long)_rejGarde,
                     (unsigned long)heapLibre(), (unsigned long)blocLibre(),
                     (unsigned long)_dureePire, (unsigned long)_depassements);
    if (n < 0) { buf[0] = '\0'; return 0; }
    return ((size_t)n < len) ? (size_t)n : (len - 1);
  }

private:
  static const uint8_t GE_SLOT_AUCUN = 0xFF;
  // Anneau de GE_BUCKETS+1 seaux : un seau n'est remis à zéro qu'après
  // (GE_BUCKETS+1) rotations, donc un événement reste compté AU MOINS
  // _fenetreMs. Sans le +1, il restait compté 7/8 de fenêtre (26 250 ms
  // au lieu de 30 000) et deux rafales de 4 passaient en 26 254 ms.
  static const uint8_t GE_RING = (uint8_t)(GE_BUCKETS + 1);

  // La concurrence demandée est ramenée dans [1, GE_SLOTS] : GE_SLOTS est
  // une contrainte de compilation, pas une erreur de l'appelant.
  static uint8_t borneConcurrence(uint8_t c) {
    if (c < 1) return 1;
    return (c > GE_SLOTS) ? (uint8_t)GE_SLOTS : c;
  }

  bool admitInterne(uint8_t poids, uint32_t taille, uint8_t& idx) {
    idx = GE_SLOT_AUCUN;

    // Échantillonnage mémoire HORS verrou : couper les interruptions
    // pendant heap_caps_get_free_size() n'apporte rien et coûte (finding 41).
    uint32_t heap = heapLibre();
    uint32_t bloc = blocLibre();
    uint32_t now  = millis();

    GE_VERROU(&_mux);
    bool xoff = enPression(heap, bloc, _heapXoff, _blocXoff);
    _xoff = xoff;    // publié sur TOUTES les sorties (W4)

    if (!_actif) {
      // Protection désactivée : on laisse passer, mais le slot est FICTIF.
      // Il est réellement alloué pour que le ticket soit « vrai » et que
      // sortir() soit symétrique ; il n'est simplement pas mesuré.
      // (L'ancien compteur global _slotsSansMesure attribuait la libération
      //  à la mauvaise requête et fuyait un slot — findings 28 et 29.)
      uint8_t i = slotLibre();
      if (i != GE_SLOT_AUCUN) {
        _slotBusy[i]   = true;
        _slotFictif[i] = true;
        _slotStart[i]  = now;
        idx = i;
        _actifs++;
      }
      _admis++;
      GE_DEVERROU(&_mux);
      return true;
    }

    if (poids == 0) poids = 1;

    // 1. Cooldown post-reboot / post-erreur
    if (_cooldownFin && (int32_t)(now - _cooldownFin) < 0) {
      return refuserSousVerrou(GE_REFUS_BACKOFF);
    }

    // 2. Garde de démarrage
    if (_gardeActive && ((heap < _gardeHeap) || (bloc < _gardeBloc))) {
      return refuserSousVerrou(GE_REFUS_GARDE);
    }

    // 3. Poids au-delà du réglage (structurel)
    if (poids > _poidsMax) {
      return refuserSousVerrou(GE_REFUS_POIDS);
    }

    // 4. Pression mémoire : max effectif réduit
    uint16_t maxEff = xoff ? _sousPression : _canaux;
    if (poids > maxEff) {
      return refuserSousVerrou(xoff ? GE_REFUS_PRESSION : GE_REFUS_POIDS);
    }

    // 5. Concurrence
    if (_actifs >= _concurrence) {
      return refuserSousVerrou(GE_REFUS_CONCURRENCE);
    }

    // 6. Taille du plus gros bloc alloué
    if (taille) {
      if (taille > _bodyMax) return refuserSousVerrou(GE_REFUS_BODY);
      uint32_t part = (heap * _partHeap) / 100UL;
      if (taille > part)     return refuserSousVerrou(GE_REFUS_BODY);
    }

    // 7. Fenêtre glissante pondérée
    avancer(now);
    if ((uint32_t)(charge() + poids) > maxEff) {
      return refuserSousVerrou(GE_REFUS_FLUX);
    }

    // Admis : trouver un slot libre
    uint8_t i = slotLibre();
    if (i == GE_SLOT_AUCUN) {   // ne devrait pas arriver (contrôle 5)
      return refuserSousVerrou(GE_REFUS_CONCURRENCE);
    }

    _slotBusy[i]   = true;
    _slotFictif[i] = false;
    _slotStart[i]  = now;
    idx = i;

    _bucket[_idx] = (uint16_t)(_bucket[_idx] + poids);
    _actifs++;
    _admis++;
    _derniereActivite = now;
    _refus = GE_REFUS_AUCUN;
    GE_DEVERROU(&_mux);
    return true;
  }

  // ⚠️ Appelée VERROU TENU : elle rend le verrou elle-même.
  bool refuserSousVerrou(GE_Refus r) {
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
      default: break;
    }
    GE_DEVERROU(&_mux);
    return false;
  }

  uint8_t slotLibre() const {
    for (uint8_t i = 0; i < GE_SLOTS; i++) if (!_slotBusy[i]) return i;
    return GE_SLOT_AUCUN;
  }

  void viderFenetre(uint32_t now) {
    for (uint8_t i = 0; i < GE_RING; i++) _bucket[i] = 0;
    _idx = 0;
    _winStart = now;
    _fenetreVide = true;
  }

  void avancer(uint32_t now) {
    if (!_fenetreVide) { _winStart = now; _fenetreVide = true; return; }
    uint16_t garde = GE_RING;    // uint8_t tronquait à 256 → rotation morte
    while ((uint32_t)(now - _winStart) >= _bucketMs && garde--) {
      _winStart += _bucketMs;
      _idx = (uint8_t)((_idx + 1) % GE_RING);
      _bucket[_idx] = 0;
    }
    if ((uint32_t)(now - _winStart) >= _fenetreMs) {
      for (uint8_t i = 0; i < GE_RING; i++) _bucket[i] = 0;
      _winStart = now;
    }
  }

  uint16_t charge() const {
    uint32_t s = 0;
    for (uint8_t i = 0; i < GE_RING; i++) s += _bucket[i];
    return (s > 0xFFFFUL) ? 0xFFFF : (uint16_t)s;
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

  // ── Fenêtre (anneau GE_BUCKETS+1) ─────────────────────────────
  uint16_t _bucket[GE_BUCKETS + 1] = {};   // initialisé (C5)
  uint32_t _winStart     = 0;
  uint8_t  _idx          = 0;
  bool     _fenetreVide  = false;

  // ── Slots de concurrence (un temps de départ PAR slot) ────────
  uint32_t _slotStart[GE_SLOTS] = {};
  bool     _slotBusy[GE_SLOTS]  = {};
  bool     _slotFictif[GE_SLOTS] = {};
  uint8_t  _slotDernier  = GE_SLOT_AUCUN;
  uint8_t  _actifs       = 0;

  // ── Backoff ───────────────────────────────────────────────────
  uint32_t _backoffBase = GE_BACKOFF_BASE_DEF;
  uint32_t _backoffMax  = GE_BACKOFF_MAX_DEF;
  uint32_t _backoffCur  = GE_BACKOFF_BASE_DEF;
  uint32_t _cooldownFin = 0;

  // ── Inactivité ────────────────────────────────────────────────
  uint32_t _derniereActivite = 0;

  // ── Compteurs ─────────────────────────────────────────────────
  uint32_t _admis          = 0;
  uint32_t _rejetes        = 0;
  uint32_t _rejFlux        = 0;
  uint32_t _rejPoids       = 0;
  uint32_t _rejPression    = 0;
  uint32_t _rejConcurrence = 0;
  uint32_t _rejBody        = 0;
  uint32_t _rejBackoff     = 0;
  uint32_t _rejGarde       = 0;
  uint32_t _dureePire      = 0;
  uint32_t _depassements   = 0;

  // ── État ──────────────────────────────────────────────────────
  GE_Refus    _refus   = GE_REFUS_AUCUN;
  GE_ProfilId _profil  = GE_PROFIL_AUTO;
  bool        _actif   = true;
  bool        _xoff    = false;

  // ── Routes ────────────────────────────────────────────────────
  GE_RouteStat _routes[GE_ROUTES > 0 ? GE_ROUTES : 1] = {};
#if GE_ROUTE_NOM_MAX > 0
  char _routeNoms[GE_ROUTES > 0 ? GE_ROUTES : 1][GE_ROUTE_NOM_MAX] = {};
#endif
  int8_t _routeCur       = -1;
  bool   _routesSaturees = false;

#if defined(ESP32)
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
#endif
};

// Définition du destructeur de ticket (Guard_ESP est maintenant complet).
inline void GE_Ticket::liberer() {
  if (_g && _idx != GE_SLOT_AUCUN) _g->sortir(_idx);
  _g = nullptr;
  _idx = GE_SLOT_AUCUN;
}

// Empreinte : un garde-fou qui grossit devient lui-même un risque mémoire.
static_assert(sizeof(Guard_ESP) < 300, "sizeof(Guard_ESP) doit rester < 300 octets");

static_assert(GE_SOUS_PRESSION_DEF >= 1, "GE_SOUS_PRESSION_DEF doit etre >= 1");
static_assert(GE_POIDS_MAX_DEF >= 1,     "GE_POIDS_MAX_DEF doit etre >= 1");
static_assert(GE_CONCURRENCE_DEF >= 1,   "GE_CONCURRENCE_DEF doit etre >= 1");

// Instance globale prête à l'emploi (une seule par firmware).
inline Guard_ESP& guardInstance() {
  static Guard_ESP g;
  return g;
}

#endif // GUARD_ESP_H
