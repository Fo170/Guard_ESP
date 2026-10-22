// ════════════════════════════════════════════════════════════════
// Tests hôte de Guard_ESP — valident la LOGIQUE sans matériel.
// Chaque test référence le problème réel qu'il protège (taxonomie
// du livre blanc) : si un test casse, un incident du parc peut revenir.
//
// Compilation : g++ -std=c++17 -I tests -I src tests/test_guard.cpp
// ════════════════════════════════════════════════════════════════
#include "arduino_shim.h"
#include "Guard_ESP.h"
#include <assert.h>
#include <stdio.h>

uint32_t g_fakeMillis = 1;
uint32_t g_fakeHeap   = 4700;
uint32_t g_fakeBloc   = 3700;
FakeESP  ESP;

static int g_total = 0;
static int g_ok    = 0;

static void check(const char* nom, bool cond) {
  g_total++;
  if (cond) { g_ok++; printf("  OK   %s\n", nom); }
  else      { printf("  ECHEC %s\n", nom); }
}

// Remet un guard propre pour chaque test.
static void reset(Guard_ESP& g) {
  g_fakeMillis = 1;
  g_fakeHeap   = 4700;
  g_fakeBloc   = 3700;
  g = Guard_ESP();
  g.profil(GE_PROFIL_ESP8266);
  g.demarrer();
  g_fakeMillis = 100000;  // hors cooldown de démarrage
}

// ── T1 : fenêtre pondérée (rafale MCP → 429) ────────────────────
static void t1_fenetre() {
  printf("\nT1 — fenetre glissante ponderee (problème : rafale MCP, panic 31/08)\n");
  Guard_ESP g; reset(g);
  int admis = 0;
  for (int i = 0; i < 10; i++) if (g.admit(1)) { admis++; g.sortir(); }
  check("ESP8266 : 4 requetes admises sur 10 (profil A)", admis == 4);
  check("cause = FLUX", g.refus() == GE_REFUS_FLUX);
  check("retryAfter = fenetre (30 s)", g.retryAfterMs() == 30000);

  // Après la fenêtre, le crédit revient.
  g_fakeMillis += 31000;
  check("credit rendu apres la fenetre", g.admit(1) == true);
  g.sortir();
}

// ── T2 : pondération des outils ─────────────────────────────────
static void t2_poids() {
  printf("\nT2 — ponderation (problème : outil lourd compte comme un leger)\n");
  Guard_ESP g; reset(g);
  check("poids 3 admis", g.admit(3) == true);
  g.sortir();
  check("2e poids 3 refuse (3+3 > 4)", g.admit(3) == false);
  check("cause = FLUX", g.refus() == GE_REFUS_FLUX);
}

// ── T3 : XON/XOFF mémoire ───────────────────────────────────────
static void t3_pression() {
  printf("\nT3 — backpressure memoire (problème : seuil 35 Ko = XOFF permanent)\n");
  Guard_ESP g; reset(g);
  check("regime normal : pas de XOFF", g.enPression() == false);

  g_fakeHeap = 4000;  // sous 4200
  check("heap 4000 < XOFF 4200 → pression", g.enPression() == true);
  check("poids 3 refuse sous pression", g.admit(3) == false);
  check("cause = PRESSION (pas POIDS)", g.refus() == GE_REFUS_PRESSION);
  check("poids 1 encore admis sous pression", g.admit(1) == true);

  g_fakeHeap = 4700;
  g_fakeBloc = 1500;  // sous 1800 : fragmentation seule
  check("bloc contigu 1500 < 1800 → pression", g.enPression() == true);
}

// ── T4 : concurrence (ESP8266 mono-client) ──────────────────────
static void t4_concurrence() {
  printf("\nT4 — concurrence (problème : ESP8266WebServer mono-client, gel 10 s)\n");
  Guard_ESP g; reset(g);
  check("1re requete admise", g.admit(1) == true);
  check("2e simultanee refusee (concurrence 1)", g.admit(1) == false);
  check("cause = CONCURRENCE", g.refus() == GE_REFUS_CONCURRENCE);
  g.sortir();
  check("apres sortir() une requete repasse", g.admit(1) == true);
}

// ── T5 : plafond de taille de réponse ───────────────────────────
static void t5_body() {
  printf("\nT5 — plafond de reponse (problème : MCP_MAX_BODY 8192 sur 4,7 Ko)\n");
  Guard_ESP g; reset(g);
  check("taille 1000 admise", g.admit(1, 1000) == true);
  g.sortir();
  check("taille 4000 refusee (body max 2048)", g.admit(1, 4000) == false);
  check("cause = BODY", g.refus() == GE_REFUS_BODY);
}

// ── T6 : garde de démarrage ─────────────────────────────────────
static void t6_garde() {
  printf("\nT6 — garde de demarrage (problème : MCP demarre a 4,3 Ko)\n");
  Guard_ESP g; reset(g);
  g.setGarde(4500, 3000);
  g_fakeHeap = 4300;
  check("heap 4300 < garde 4500 → refus", g.admit(1) == false);
  check("cause = GARDE", g.refus() == GE_REFUS_GARDE);
  g_fakeHeap = 4700;
  check("heap 4700 >= garde → admis", g.admit(1) == true);
  g.sortir();
}

// ── T7 : backoff / cooldown ─────────────────────────────────────
static void t7_backoff() {
  printf("\nT7 — backoff (problème : tempete de retries)\n");
  Guard_ESP g; reset(g);
  g.setBackoff(1000, 30000);
  g_fakeHeap = 4700;
  g.demarrer();   // re-arme le cooldown au temps courant
  check("cooldown de demarrage actif", g.admit(1) == false);
  check("cause = BACKOFF", g.refus() == GE_REFUS_BACKOFF);
  g_fakeMillis += 61000;
  check("apres 61 s le service reprend", g.admit(1) == true);
  g.sortir();
}

// ── T8 : dépassement de durée → cooldown ────────────────────────
static void t8_duree() {
  printf("\nT8 — garde de duree (problème : WDT reset n°7)\n");
  Guard_ESP g; reset(g);
  g.setBackoff(1000, 30000);
  g.setInactivite(60000);
  check("admis", g.admit(1) == true);
  g_fakeMillis += 5000;  // > dureeMax 800 ms
  g.sortir();
  check("depassement compte", g.depassements() == 1);
  check("duree pire enregistree", g.dureePire() >= 5000);
}

// ── T9 : watchdog d'inactivité ──────────────────────────────────
static void t9_inactivite() {
  printf("\nT9 — watchdog d'inactivite (problème : Domoticz muet 9 h 34)\n");
  Guard_ESP g; reset(g);
  g.setInactivite(300000);
  g.activite();
  check("actif au depart", g.inactif() == false);
  g_fakeMillis += 400000;
  check("inactif apres 400 s sans activite", g.inactif() == true);
  g.activite();
  check("activite() remet le compteur", g.inactif() == false);
}

// ── T10 : auto-calibration ──────────────────────────────────────
static void t10_calibrage() {
  printf("\nT10 — auto-calibration (problème : seuils recopies par projet)\n");
  Guard_ESP g; reset(g);
  g_fakeHeap = 4700;
  g.autoCalibrer();
  check("ESP8266 4,7 Ko → 4 canaux", g.canaux() == 4);
  check("XOFF sous le regime normal (3525 < 4700)", g.enPression() == false);

  g_fakeHeap = 76000;
  g_fakeBloc = 68972;   // bloc contigu mesure sur Telegram
  g.autoCalibrer();
  check("ESP32 76 Ko → 8 canaux", g.canaux() == 8);
  check("ESP32 concurrence 3", g.concurrence() == 3);
  check("ESP32 XOFF 57000 < 76000", g.enPression() == false);
}

// ── T11 : profils distincts par plateforme ──────────────────────
static void t11_profils() {
  printf("\nT11 — profils ESP8266 vs ESP32 (problème : 8 req/30 s sur 4,7 Ko)\n");
  Guard_ESP a; a.profil(GE_PROFIL_ESP8266);
  Guard_ESP b; b.profil(GE_PROFIL_ESP32);
  check("canaux 4 vs 8", a.canaux() == 4 && b.canaux() == 8);
  check("concurrence 1 vs 3", a.concurrence() == 1 && b.concurrence() == 3);
  check("body 2048 vs 8192", a.bodyMax() == 2048 && b.bodyMax() == 8192);
  check("duree 800 vs 2500 ms", a.dureeMaxMs() == 800 && b.dureeMaxMs() == 2500);
  check("ESP32 a >= 4x les seuils ESP8266",
        b.bodyMax() >= a.bodyMax() * 4);
}

// ── T12 : compteurs par route ───────────────────────────────────
static void t12_routes() {
  printf("\nT12 — compteurs par route (problème : HTTP 200 + corps vide)\n");
  Guard_ESP g; reset(g);
  g.route("/status"); g.routeOk();
  g.route("/status"); g.routeOk();
  g.route("/status"); g.routeKo();
  const GE_RouteStat* r = g.routes();
  check("route /status : 2 ok / 1 ko", r[0].ok == 2 && r[0].ko == 1);
}

// ── T13 : résumé texte sans String ──────────────────────────────
static void t13_resume() {
  printf("\nT13 — resume sans allocation\n");
  Guard_ESP g; reset(g);
  g.admit(1);
  char buf[320];
  size_t n = g.resume(buf, sizeof(buf));
  check("resume non vide", n > 40);
  check("resume contient le profil", strstr(buf, "ESP8266") != nullptr);
  check("resume contient les canaux", strstr(buf, "4/30s") != nullptr);
}

// ── T14 : empreinte mémoire ─────────────────────────────────────
static void t14_empreinte() {
  printf("\nT14 — empreinte (critère : < 300 octets)\n");
  printf("  sizeof(Guard_ESP) = %zu octets\n", sizeof(Guard_ESP));
  check("sizeof(Guard_ESP) < 300", sizeof(Guard_ESP) < 300);
}

int main() {
  printf("=== Tests Guard_ESP v%s ===\n", GE_VERSION);
  t1_fenetre();
  t2_poids();
  t3_pression();
  t4_concurrence();
  t5_body();
  t6_garde();
  t7_backoff();
  t8_duree();
  t9_inactivite();
  t10_calibrage();
  t11_profils();
  t12_routes();
  t13_resume();
  t14_empreinte();
  printf("\n=== %d/%d tests OK ===\n", g_ok, g_total);
  return (g_ok == g_total) ? 0 : 1;
}
