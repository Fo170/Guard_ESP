// ════════════════════════════════════════════════════════════════
// Tests hôte de Guard_ESP — valident la LOGIQUE sans matériel.
// Chaque test référence le problème réel qu'il protège (taxonomie
// du livre blanc) ou le finding du cycle 1 qu'il verrouille.
//
// Compilation : g++ -std=c++17 -I tests -I src tests/test_guard.cpp
// ════════════════════════════════════════════════════════════════
#include "arduino_shim.h"
#include "Guard_ESP.h"
#include "Guard_ESP_web.h"
#include "Guard_ESP_print.h"
#include "Guard_ESP_mcp.h"
#include "Guard_ESP_tcp.h"
#include <assert.h>
#include <stdio.h>

uint32_t g_fakeMillis = 1;
uint32_t g_fakeHeap   = 4700;
uint32_t g_fakeBloc   = 3700;
FakeESP  ESP;

int g_creations = 0;
int g_deconnexions = 0;
bool g_createEchoue = false;
std::function<void(void)> g_derniereDeconnexion;
char   g_dernierEcrit[256] = {};
size_t g_dernierEcritLen = 0;
bool   g_dernierClose = false;
bool   g_dernierAbort = false;
int    FauxTcpClient::vivants = 0;
FauxTcpClient* g_admis[8];
int    g_nbAdmis = 0;

static int g_total = 0;
static int g_ok    = 0;

static void check(const char* nom, bool cond) {
  g_total++;
  if (cond) { g_ok++; printf("  OK    %s\n", nom); }
  else      { printf("  ECHEC %s\n", nom); }
}

// Remet un guard propre pour chaque test (l'état simulé est global).
static void reset(Guard_ESP& g) {
  g_fakeMillis = 1;
  g_fakeHeap   = 4700;
  g_fakeBloc   = 3700;
  g = Guard_ESP();
  g.profil(GE_PROFIL_ESP8266);
  g.demarrer();
  g_fakeMillis = 100000;  // hors cooldown de démarrage
}

// La fenêtre est garantie >= fenetreMs (anneau GE_BUCKETS+1), donc
// le crédit revient au plus tard après (GE_BUCKETS+1)*bucketMs.
#define GE_PLEINE_MS ((GE_BUCKETS + 1) * (30000UL / GE_BUCKETS))

// ── T1 : fenêtre pondérée (rafale MCP → 429) ────────────────────
static void t1_fenetre() {
  printf("\nT1 — fenetre glissante ponderee (probleme : rafale MCP, panic 31/08)\n");
  Guard_ESP g; reset(g);
  int admis = 0;
  for (int i = 0; i < 10; i++) if (g.admit(1)) { admis++; g.sortir(); }
  check("ESP8266 : 4 requetes admises sur 10 (profil A)", admis == 4);
  check("cause = FLUX", g.refus() == GE_REFUS_FLUX);
  check("retryAfter = anneau complet (33 750 ms)", g.retryAfterMs() == GE_PLEINE_MS);

  g_fakeMillis += GE_PLEINE_MS;
  check("credit rendu apres la fenetre pleine", g.admit(1) == true);
  g.sortir();
}

// ── T2 : pondération des outils ─────────────────────────────────
static void t2_poids() {
  printf("\nT2 — ponderation (probleme : outil lourd compte comme un leger)\n");
  Guard_ESP g; reset(g);
  check("poids 3 admis", g.admit(3) == true);
  g.sortir();
  check("2e poids 3 refuse (3+3 > 4)", g.admit(3) == false);
  check("cause = FLUX", g.refus() == GE_REFUS_FLUX);
}

// ── T3 : XON/XOFF mémoire ───────────────────────────────────────
static void t3_pression() {
  printf("\nT3 — backpressure memoire (probleme : seuil 35 Ko = XOFF permanent)\n");
  Guard_ESP g; reset(g);
  check("regime normal : pas de XOFF", g.enPression() == false);

  g_fakeHeap = 4000;  // sous 4200
  check("heap 4000 < XOFF 4200 -> pression", g.enPression() == true);
  check("poids 3 refuse sous pression", g.admit(3) == false);
  check("cause = PRESSION (pas POIDS)", g.refus() == GE_REFUS_PRESSION);
  check("poids 1 encore admis sous pression", g.admit(1) == true);
  g.sortir();

  g_fakeHeap = 4700;
  g_fakeBloc = 1500;  // sous 1800 : fragmentation seule
  check("bloc contigu 1500 < 1800 -> pression", g.enPression() == true);
}

// ── T4 : concurrence (ESP8266 mono-client) ──────────────────────
static void t4_concurrence() {
  printf("\nT4 — concurrence (probleme : ESP8266WebServer mono-client, gel 10 s)\n");
  Guard_ESP g; reset(g);
  check("1re requete admise", g.admit(1) == true);
  check("2e simultanee refusee (concurrence 1)", g.admit(1) == false);
  check("cause = CONCURRENCE", g.refus() == GE_REFUS_CONCURRENCE);
  g.sortir();
  check("apres sortir() une requete repasse", g.admit(1) == true);
  g.sortir();
}

// ── T5 : plafond de taille de réponse ───────────────────────────
static void t5_body() {
  printf("\nT5 — plafond de reponse (probleme : MCP_MAX_BODY 8192 sur 4,7 Ko)\n");
  Guard_ESP g; reset(g);
  check("taille 1000 admise", g.admit(1, 1000) == true);
  g.sortir();
  check("taille 4000 refusee (body max 2048)", g.admit(1, 4000) == false);
  check("cause = BODY", g.refus() == GE_REFUS_BODY);
}

// ── T6 : garde de démarrage ─────────────────────────────────────
static void t6_garde() {
  printf("\nT6 — garde de demarrage (probleme : MCP demarre a 4,3 Ko)\n");
  Guard_ESP g; reset(g);
  g.setGarde(4500, 3000);
  g_fakeHeap = 4300;
  check("heap 4300 < garde 4500 -> refus", g.admit(1) == false);
  check("cause = GARDE", g.refus() == GE_REFUS_GARDE);
  g_fakeHeap = 4700;
  check("heap 4700 >= garde -> admis", g.admit(1) == true);
  g.sortir();
}

// ── T7 : backoff / cooldown ─────────────────────────────────────
static void t7_backoff() {
  printf("\nT7 — backoff (probleme : tempete de retries)\n");
  Guard_ESP g; reset(g);
  g.setBackoff(1000, 30000);
  g_fakeHeap = 4700;
  g.demarrer();   // re-arme le cooldown au temps courant
  check("cooldown de demarrage actif", g.admit(1) == false);
  check("cause = BACKOFF", g.refus() == GE_REFUS_BACKOFF);
  check("retryAfter = cooldown restant", g.retryAfterMs() > 0 && g.retryAfterMs() <= 60000);
  g_fakeMillis += 61000;
  check("apres 61 s le service reprend", g.admit(1) == true);
  g.sortir();
}

// ── T8 : dépassement de durée → cooldown ────────────────────────
static void t8_duree() {
  printf("\nT8 — garde de duree (probleme : WDT reset n°7)\n");
  Guard_ESP g; reset(g);
  g.setBackoff(1000, 30000);
  check("admis", g.admit(1) == true);
  g_fakeMillis += 5000;  // > dureeMax 800 ms
  g.sortir();
  check("depassement compte", g.depassements() == 1);
  check("duree pire enregistree", g.dureePire() >= 5000);
  check("cooldown arme apres depassement", g.admit(1) == false);
  check("cause = BACKOFF", g.refus() == GE_REFUS_BACKOFF);
}

// ── T9 : watchdog d'inactivité ──────────────────────────────────
static void t9_inactivite() {
  printf("\nT9 — watchdog d'inactivite (probleme : Domoticz muet 9 h 34)\n");
  Guard_ESP g; reset(g);
  g.setInactivite(300000);
  g.activite();
  check("actif au depart", g.inactif() == false);
  g_fakeMillis += 400000;
  check("inactif apres 400 s sans activite", g.inactif() == true);
  g.activite();
  check("activite() remet le compteur", g.inactif() == false);
  g.setInactivite(0);
  g_fakeMillis += 900000;
  check("setInactivite(0) desactive le watchdog", g.inactif() == false);
}

// ── T10 : auto-calibration ──────────────────────────────────────
static void t10_calibrage() {
  printf("\nT10 — auto-calibration (probleme : seuils recopies par projet)\n");
  Guard_ESP g; reset(g);
  g_fakeHeap = 4700;
  g_fakeBloc = 3700;
  g.autoCalibrer();
  check("ESP8266 4,7 Ko -> 4 canaux", g.canaux() == 4);
  check("XOFF sous le regime normal (3525 < 4700)", g.enPression() == false);
  check("concurrence 1 sur ESP8266", g.concurrence() == 1);

  g_fakeHeap = 76000;
  g_fakeBloc = 68972;   // bloc contigu mesure sur Telegram
  g.autoCalibrer();
  check("ESP32 76 Ko -> 8 canaux", g.canaux() == 8);
  check("ESP32 concurrence 3", g.concurrence() == 3);
  check("ESP32 XOFF 57000 < 76000", g.enPression() == false);
}

// ── T11 : profils distincts par plateforme ──────────────────────
static void t11_profils() {
  printf("\nT11 — profils ESP8266 vs ESP32 (probleme : 8 req/30 s sur 4,7 Ko)\n");
  Guard_ESP a; reset(a); a.profil(GE_PROFIL_ESP8266);
  Guard_ESP b; reset(b); b.profil(GE_PROFIL_ESP32);
  check("canaux 4 vs 8", a.canaux() == 4 && b.canaux() == 8);
  check("concurrence 1 vs 3", a.concurrence() == 1 && b.concurrence() == 3);
  check("body 2048 vs 8192", a.bodyMax() == 2048 && b.bodyMax() == 8192);
  check("duree 800 vs 2500 ms", a.dureeMaxMs() == 800 && b.dureeMaxMs() == 2500);
  check("poidsMax 3 dans les deux profils", a.poidsMax() == 3 && b.poidsMax() == 3);
  check("ESP32 a >= 4x les seuils ESP8266", b.bodyMax() >= a.bodyMax() * 4);
  check("profils arment la garde de demarrage", a.heapRefus() == 3000 && b.heapRefus() == 20000);
}

// ── T12 : compteurs par route ───────────────────────────────────
static void t12_routes() {
  printf("\nT12 — compteurs par route (probleme : HTTP 200 + corps vide)\n");
  Guard_ESP g; reset(g);
  g.route("/status"); g.routeOk();
  g.route("/status"); g.routeOk();
  g.route("/status"); g.routeKo();
  const GE_RouteStat* r = g.routes();
  check("route /status : 2 ok / 1 ko", r[0].ok == 2 && r[0].ko == 1);
  check("table non saturee", g.routesSaturees() == false);
}

// ── T13 : résumé texte sans String ──────────────────────────────
static void t13_resume() {
  printf("\nT13 — resume sans allocation\n");
  Guard_ESP g; reset(g);
  if (g.admit(1)) g.sortir();
  char buf[400];
  size_t n = g.resume(buf, sizeof(buf));
  check("resume non vide", n > 40);
  check("resume contient le profil", strstr(buf, "ESP8266") != nullptr);
  check("resume contient les canaux", strstr(buf, "4/30s") != nullptr);
}

// ── T14 : empreinte mémoire ─────────────────────────────────────
static void t14_empreinte() {
  printf("\nT14 — empreinte (critere : < 300 octets)\n");
  printf("  sizeof(Guard_ESP) = %zu octets\n", sizeof(Guard_ESP));
  check("sizeof(Guard_ESP) < 300", sizeof(Guard_ESP) < 300);
  check("sizeof(GE_Ticket) <= 16", sizeof(GE_Ticket) <= 16);
}

// ── T15 (C1) : la fenêtre ne laisse plus passer 2x le quota ─────
static void t15_double_rafale() {
  printf("\nT15 (C1) — double rafale (defaut : 8 requetes en 26 254 ms)\n");
  Guard_ESP g; reset(g);
  // 1er lot à la FIN du premier seau (t = bucketMs - 1)
  g_fakeMillis = 100000 + (30000UL / GE_BUCKETS) - 1;
  int lot1 = 0;
  for (int i = 0; i < 4; i++) if (g.admit(1)) { lot1++; g.sortir(); }
  check("1er lot : 4 admis", lot1 == 4);
  // 2e lot 26 254 ms plus tard (ancien defaut : les 4 du 1er lot etaient oublies)
  g_fakeMillis += 26254;
  int lot2 = 0;
  for (int i = 0; i < 4; i++) if (g.admit(1)) { lot2++; g.sortir(); }
  check("2e lot refuse (fenetre garantie >= 30 s)", lot2 == 0);
  check("cause = FLUX", g.refus() == GE_REFUS_FLUX);
  // Apres la fenetre pleine, le credit revient
  g_fakeMillis += GE_PLEINE_MS;
  check("credit rendu apres 33,75 s", g.admit(1) == true);
  g.sortir();
}

// ── T16 (C2) : deux tickets libèrent deux slots ─────────────────
static void t16_tickets() {
  printf("\nT16 (C2) — fuite de slot web (defaut : actifs=1 apres 2 admits + 2 fins)\n");
  Guard_ESP g; reset(g);
  g.profil(GE_PROFIL_ESP32);      // concurrence 3
  g_fakeHeap = 76000; g_fakeBloc = 68972;
  {
    GE_Ticket a = g.ticket(1);
    GE_Ticket b = g.ticket(1);
    check("2 tickets ouverts", a.ok() && b.ok());
    check("2 slots occupes", g.actifs() == 2);
  }
  check("les 2 slots sont rendus a la destruction", g.actifs() == 0);
  GE_Ticket c = g.ticket(1);
  check("un 3e ticket repasse", c.ok());
}

// ── T17 (C4) : !_actif ne pollue pas les compteurs ──────────────
static void t17_inactif() {
  printf("\nT17 (C4) — protection desactivee (defaut : dureePire=105000)\n");
  Guard_ESP g; reset(g);
  g.actif(false);
  check("admis malgre _actif=false", g.admit(1) == true);
  check("slot fictif alloue (liberation symetrique)", g.actifs() == 1);
  g_fakeMillis += 5000;
  g.sortir();
  check("dureePire reste a 0", g.dureePire() == 0);
  check("aucun depassement fantome", g.depassements() == 0);
  check("slot fictif rendu", g.actifs() == 0);
  g.actif(true);
  check("reactivation : la fenetre reprend", g.admit(1) == true);
  g.sortir();
}

// ── T18 (C5) : _bucket initialisé ───────────────────────────────
static void t18_init() {
  printf("\nT18 (C5) — etat initial (defaut : charge=65535 sans demarrer())\n");
  g_fakeMillis = 100000;
  g_fakeHeap = 4700;
  g_fakeBloc = 3700;
  Guard_ESP g;              // AUCUN demarrer()
  g.profil(GE_PROFIL_ESP8266);
  check("charge initiale nulle", g.chargeCourante() == 0);
  check("admit fonctionne sans demarrer()", g.admit(1) == true);
  g.sortir();
}

// ── T19 (W1) : sortir() sans admit() ────────────────────────────
static void t19_sortir_orphelin() {
  printf("\nT19 (W1) — sortir() sans admit() (defaut : dureePire=millis)\n");
  Guard_ESP g; reset(g);
  g.sortir();
  g.sortir();
  check("aucune duree fantome", g.dureePire() == 0);
  check("aucun depassement fantome", g.depassements() == 0);
}

// ── T20 (W2) : un temps de départ par slot ──────────────────────
static void t20_slots() {
  printf("\nT20 (W2) — temps de depart par slot (defaut : duree sous-estimee)\n");
  Guard_ESP g; reset(g);
  g.profil(GE_PROFIL_ESP32);
  g_fakeHeap = 76000; g_fakeBloc = 68972;
  GE_Ticket a = g.ticket(1);
  g_fakeMillis += 100;
  GE_Ticket b = g.ticket(1);
  g_fakeMillis += 700;
  b = GE_Ticket();            // libere b : 800 ms mesurees
  check("duree du slot long mesuree (700 ms)", g.dureePire() == 700);
  g_fakeMillis += 100;
  a = GE_Ticket();            // libere a : 900 ms
  check("duree du 1er slot conservee", g.dureePire() == 900);
}

// ── T21 (W3) : tick() puis sortir() ne recompte pas ─────────────
static void t21_tick() {
  printf("\nT21 (W3) — tick() + sortir() (defaut : depassement compte 2 fois)\n");
  Guard_ESP g; reset(g);
  check("admis", g.admit(1) == true);
  g_fakeMillis += 10000;      // > 4 x 800 ms
  g.tick();
  check("slot libere par tick()", g.actifs() == 0);
  check("un seul depassement", g.depassements() == 1);
  g.sortir();
  check("sortir() ne recompte pas", g.depassements() == 1);
}

// ── T22 (W4) : _xoff à jour sur toutes les sorties ──────────────
static void t22_xoff() {
  printf("\nT22 (W4) — _xoff perime (defaut : xoff=false sous pression)\n");
  Guard_ESP g; reset(g);
  g_fakeHeap = 4000;          // sous XOFF 4200
  check("refus BODY sous pression", g.admit(1, 4000) == false);
  check("cause = BODY", g.refus() == GE_REFUS_BODY);
  check("xoff() reflete la pression", g.xoff() == true);

  g_fakeHeap = 4700; g_fakeBloc = 3700;
  Guard_ESP k; reset(k);
  k.profil(GE_PROFIL_ESP8266);
  check("refus POIDS structurel hors pression", k.admit(9) == false);
  check("cause = POIDS", k.refus() == GE_REFUS_POIDS);
  check("xoff=false hors pression", k.xoff() == false);
}

// ── T23 (W6) : contrat de resume() ──────────────────────────────
static void t23_resume_contrat() {
  printf("\nT23 (W6) — contrat de resume (defaut : retournait 191 pour buf[20])\n");
  Guard_ESP g; reset(g);
  char petit[20];
  size_t n = g.resume(petit, sizeof(petit));
  check("retour <= len-1", n <= sizeof(petit) - 1);
  check("chaine terminee", petit[n] == '\0');
  check("len=0 -> retour 0", g.resume(petit, 0) == 0);
  check("nullptr -> retour 0", g.resume(nullptr, 20) == 0);
}

// ── T24 (W7) : le callback de pression suit l'instance ──────────
static void t24_instance() {
  printf("\nT24 (W7) — instance du callback de pression\n");
  Guard_ESP g; reset(g);
  g_fakeHeap = 4700; g_fakeBloc = 3700;
  g.profil(GE_PROFIL_ESP8266);
  bool avecG = GE_PressionCb(&g);
  check("instance ESP8266 hors pression -> false", avecG == false);

  Guard_ESP autre; reset(autre);
  g_fakeHeap = 76000; g_fakeBloc = 68972;   // apres reset(), qui remet 4,7 Ko
  autre.profil(GE_PROFIL_ESP32);            // XOFF 25000 : hors pression a 76 Ko
  bool avecAutre = GE_PressionCb(&autre);
  check("instance ESP32 hors pression -> false", avecAutre == false);

  g_fakeHeap = 4000;
  check("instance ESP8266 sous pression -> true", GE_PressionCb(&g) == true);
}

// ── T25 (W8) : pas d'en-tête sous pression mémoire ──────────────
static void test25_headers() {
  printf("\nT25 (W8) — en-tetes HTTP sous pression memoire\n");
  Guard_ESP g; reset(g);
  g.profil(GE_PROFIL_ESP8266);
  g.demarrer();
  g_fakeMillis = 200000;                 // hors cooldown

  // Hors pression : un refus emet les 6 en-tetes de quota
  g_fakeHeap = 4700; g_fakeBloc = 3700;
  FauxServeur s1;
  GE_WebGuard<FauxServeur> wg1(g);
  for (int i = 0; i < 6; i++) { GE_Ticket t = wg1.admit(s1, 1, 0); }
  check("en-tetes emis hors pression", s1.entetes > 0);
  check("code 429 pour une rafale", s1.dernierCode == 429);

  // Sous pression : AUCUN en-tete (pas de String au pire moment)
  Guard_ESP g2; reset(g2);
  g2.profil(GE_PROFIL_ESP8266);
  g2.demarrer();
  g_fakeMillis = 200000;
  g_fakeHeap = 4000;                     // < XOFF 4200
  FauxServeur s2;
  GE_WebGuard<FauxServeur> wg2(g2);
  for (int i = 0; i < 6; i++) { GE_Ticket t = wg2.admit(s2, 1, 0); }
  check("aucun en-tete sous pression", s2.entetes == 0);
  check("un refus a bien eu lieu", s2.dernierCode != 0);
}

// ── T26 (W9) : l'adaptateur async compile et refuse ─────────────
static void test26_async() {
  printf("\nT26 (W9) — adaptateur AsyncWebServer\n");
  Guard_ESP g; reset(g);
  FauxAsyncRequest req;
  GE_AsyncWebGuard<FauxAsyncRequest> ag(g);
  GE_Ticket t = ag.admit(&req, 1, 0);
  check("requete admise", t.ok());
  check("refus en concurrence 1", ag.admit(&req, 1, 0).ok() == false);
  check("code HTTP envoye", req.dernierCode == 503 || req.dernierCode == 429);
  t = GE_Ticket();
  check("slot rendu", g.actifs() == 0);
}

// ── T27 (W10) : poidsMax / garde effectivement appliqués ────────
static void t27_reglages_actifs() {
  printf("\nT27 (W10) — reglages annonces mais inoperants\n");
  Guard_ESP g; reset(g);
  g.regler(4, 30000, 2, 1, 1, 800, 2048, 4200, 1800, 3000, 1200, 25);
  check("poidsMax=2 : poids 3 refuse", g.admit(3) == false);
  check("cause = POIDS", g.refus() == GE_REFUS_POIDS);
  check("poids 2 admis", g.admit(2) == true);
  g.sortir();
}

// ── T28 (W11) : routes comparées par chaîne ─────────────────────
static void t28_routes_chaine() {
  printf("\nT28 (W11) — routes confondues par comparaison de pointeurs\n");
  Guard_ESP g; reset(g);
  char b1[16]; char b2[16];
  strcpy(b1, "/x"); strcpy(b2, "/x");   // adresses distinctes, meme contenu
  g.route(b1); g.routeOk();
  g.route(b2); g.routeOk();
  const GE_RouteStat* r = g.routes();
  check("meme contenu -> une seule entree", r[0].ok == 2);
  check("pas de 2e entree", r[1].nom == nullptr);

  // saturation : 4 routes max
  g.route("/a"); g.route("/b"); g.route("/c"); g.route("/d");
  check("table saturee signalee", g.routesSaturees() == true);
}

// ── T29 (W12) : formules d'auto-calibration exactes ─────────────
static void t29_formules() {
  printf("\nT29 (W12) — formules autoCalibrer (defaut : garde a 0,14·H)\n");
  Guard_ESP g; reset(g);
  g_fakeHeap = 4700; g_fakeBloc = 3700;
  g.autoCalibrer();
  // heapRefus = 0,55 x 4700 = 2585 ; blocRefus = 0,25 x 4700 = 1175
  check("garde heap = 0,55 x H_ref", g.heapRefus() == 2585);
  check("garde bloc = 0,25 x H_ref", g.blocRefus() == 1175);
  check("4 canaux pour 4,7 Ko", g.canaux() == 4);
}

// ── T30 (W13) : regler() rejette les valeurs hors bornes ────────
static void t30_bornes() {
  printf("\nT30 (W13) — regler() tronque (defaut : concurrence 300 -> 44)\n");
  Guard_ESP g; reset(g);
  uint16_t avant = g.canaux();
  check("concurrence 300 refusee", g.regler(4, 30000, 3, 1, 255, 800, 2048, 4200, 1800, 3000, 1200, 25) == false);
  check("partHeap 200 refusee",  g.regler(4, 30000, 3, 1, 1, 800, 2048, 4200, 1800, 3000, 1200, 200) == false);
  check("fenetre 100 ms refusee", g.regler(4, 100, 3, 1, 1, 800, 2048, 4200, 1800, 3000, 1200, 25) == false);
  check("configuration conservee", g.canaux() == avant);
  check("reglage valide accepte", g.regler(6, 30000, 3, 1, 1, 800, 2048, 4200, 1800, 3000, 1200, 25) == true);
  check("valeur appliquee", g.canaux() == 6);
}

// ── T31 (W14) : Print streamé ───────────────────────────────────
static void t31_print() {
  printf("\nT31 (W14) — Print streame (defaut : destructeur ne flush pas)\n");
  FauxPrint cible;
  {
    GE_ChunkPrint p(cible);
    p.print("abc");
    check("rien envoye avant flush", cible.total == 0);
  }                              // destructeur -> doit vider
  check("destructeur a vide le tampon", cible.total == 3);
  check("contenu correct", cible.tampon[0] == 'a' && cible.tampon[2] == 'c');

  FauxPrint cible2;
  GE_ChunkPrint q(cible2);
  q.print("1234567890");
  q.flush();
  check("flush explicite", cible2.total == 10);
}

// ── T32 (W16) : l'exemple web passe avec 4,7 Ko de heap ─────────
static void t32_exemple() {
  printf("\nT32 (W16) — exemple 01 (defaut : taille=1200 refuse en BODY)\n");
  Guard_ESP g; reset(g);
  g.profil(GE_PROFIL_ESP8266);
  g_fakeHeap = 4700;
  // Taille = PLUS GROS BLOC alloue (192 o pour GE_ServerPrint), pas le corps total
  check("page streamee admise (taille=192)", g.admit(2, 192) == true);
  g.sortir();
  check("corps entier 1200 refuse (part du heap)", g.admit(2, 1200) == false);
}

// ── T33 : crédit restant (X-RateLimit-Remaining) ────────────────
static void t33_credit() {
  printf("\nT33 (S4) — credit restant\n");
  Guard_ESP g; reset(g);
  check("credit initial = 4", g.creditRestant() == 4);
  if (g.admit(1)) g.sortir();
  check("credit apres 1 requete = 3", g.creditRestant() == 3);
  if (g.admit(2)) g.sortir();
  check("credit apres poids 2 = 1", g.creditRestant() == 1);
}

// ── T34 : compteurs globaux ─────────────────────────────────────
static void t34_compteurs() {
  printf("\nT34 — compteurs globaux\n");
  Guard_ESP g; reset(g);
  if (g.admit(1)) g.sortir();
  if (g.admit(1)) g.sortir();
  for (int i = 0; i < 5; i++) { if (g.admit(1)) g.sortir(); }   // sature la fenetre
  check("4 admis (2 + 2 avant saturation)", g.admis() == 4);
  check("3 rejets (fenetre pleine)", g.rejetes() == 3);
  check("rejets(FLUX) == 3", g.rejets(GE_REFUS_FLUX) == 3);
  check("somme des causes == rejetes",
        g.rejets(GE_REFUS_FLUX) + g.rejets(GE_REFUS_POIDS) + g.rejets(GE_REFUS_PRESSION) +
        g.rejets(GE_REFUS_CONCURRENCE) + g.rejets(GE_REFUS_BODY) +
        g.rejets(GE_REFUS_BACKOFF) + g.rejets(GE_REFUS_GARDE) == g.rejetes());
}

// ── T35 : refusTexte couvre toutes les causes ───────────────────
static void t35_refus_texte() {
  printf("\nT35 — refusTexte\n");
  check("AUCUN", strcmp(Guard_ESP::refusTexte(GE_REFUS_AUCUN), "aucun") == 0);
  check("FLUX", strcmp(Guard_ESP::refusTexte(GE_REFUS_FLUX), "flux") == 0);
  check("POIDS", strcmp(Guard_ESP::refusTexte(GE_REFUS_POIDS), "poids") == 0);
  check("PRESSION", strcmp(Guard_ESP::refusTexte(GE_REFUS_PRESSION), "pression") == 0);
  check("CONCURRENCE", strcmp(Guard_ESP::refusTexte(GE_REFUS_CONCURRENCE), "concurrence") == 0);
  check("BODY", strcmp(Guard_ESP::refusTexte(GE_REFUS_BODY), "body") == 0);
  check("BACKOFF", strcmp(Guard_ESP::refusTexte(GE_REFUS_BACKOFF), "backoff") == 0);
  check("GARDE", strcmp(Guard_ESP::refusTexte(GE_REFUS_GARDE), "garde") == 0);
}

// ── T36 (findings 28/29) : ticket sur garde inactif ─────────────
static void t36_ticket_inactif() {
  printf("\nT36 (findings 28/29) — ticket sur garde inactif\n");
  Guard_ESP g; reset(g);
  g.actif(false);
  {
    GE_Ticket t = g.ticket(1);
    check("ticket d'un garde inactif est VRAI", t.ok() == true);
    check("slot fictif occupe", g.actifs() == 1);
  }
  check("slot rendu a la destruction", g.actifs() == 0);
  check("aucune duree mesuree", g.dureePire() == 0);

  // Bascule _actif entre admit et sortir : la liberation reste attribuee
  // au bon slot (l'ancien compteur global l'attribuait a une autre requete).
  g.profil(GE_PROFIL_ESP32);          // concurrence 3
  g_fakeHeap = 76000; g_fakeBloc = 68972;
  g_fakeMillis = 200000;
  g.actif(false);
  GE_Ticket a = g.ticket(1);
  g.actif(true);
  GE_Ticket b = g.ticket(1);          // slot REEL
  check("2 slots occupes (1 fictif + 1 reel)", g.actifs() == 2);
  a = GE_Ticket();                    // rend le fictif
  check("apres liberation du fictif : 1 slot", g.actifs() == 1);
  b = GE_Ticket();                    // rend le reel
  check("apres liberation du reel : 0 slot", g.actifs() == 0);
  check("aucun depassement fantome", g.depassements() == 0);
}

// ── T37 (finding 30) : tick() ne contourne pas le plafond ───────
static void t37_tick_concurrence() {
  printf("\nT37 (finding 30) — tick() et plafond de concurrence\n");
  Guard_ESP g; reset(g);
  g.profil(GE_PROFIL_ESP32);          // concurrence 3
  g_fakeHeap = 76000; g_fakeBloc = 68972;
  g_fakeMillis = 200000;
  GE_Ticket a = g.ticket(1);
  GE_Ticket b = g.ticket(1);
  check("2 slots ouverts", g.actifs() == 2);
  g_fakeMillis += 20000;              // tous perimes
  g.tick();
  check("tick() libere les 2 slots", g.actifs() == 0);
  // Le plafond doit rester 3, pas davantage
  GE_Ticket c = g.ticket(1);
  GE_Ticket d = g.ticket(1);
  GE_Ticket e = g.ticket(1);
  GE_Ticket f = g.ticket(1);
  check("3 slots max, le 4e refuse", c.ok() && d.ok() && e.ok() && !f.ok());
  check("cause = CONCURRENCE", g.refus() == GE_REFUS_CONCURRENCE);
}

// ── T38 : plafond TCP — le refus a lieu AVANT toute allocation ──
// Protège : reboot loop du 02/10/2026 (alloc 32 o refusée dans
// _parseReqHeader() → bad_alloc → panic). Un refus DANS le handler est
// déjà trop tard : la requête a été allouée.
// ⚠️ Les faux clients sont créés par `new` : le garde fait `delete c` sur
//    le chemin de refus, comme le framework avec un vrai AsyncClient.
static void t38_plafond_tcp() {
  printf("\nT38 — plafond TCP : refus avant allocation (OOM 02/10/2026)\n");
  Guard_ESP g; reset(g);
  FauxTcpServeur srv;
  GE_TcpGuard<FauxTcpServeur, FauxTcpClient, FauxTcpRequest> tcp(g, srv, 3);
  tcp.installer();
  check("installer() pose le handler onClient", srv.installations == 1);

  g_creations = 0; g_deconnexions = 0;
  g_dernierEcrit[0] = '\0'; g_dernierClose = false; g_dernierAbort = false;

  FauxTcpClient* c1 = new FauxTcpClient();
  FauxTcpClient* c2 = new FauxTcpClient();
  FauxTcpClient* c3 = new FauxTcpClient();
  srv.connecter(c1); srv.connecter(c2); srv.connecter(c3);
  check("3 connexions admises", tcp.actifs() == 3);
  check("3 requetes creees", g_creations == 3);
  check("setRxTimeout(3) reproduit le framework", c1->rxTimeout == GE_TCP_RX_TIMEOUT);
  check("onDisconnect arme sur chaque requete", g_deconnexions == 3);

  srv.connecter(new FauxTcpClient());   // 4e : refusée puis détruite
  check("4e refusee : AUCUNE requete creee", g_creations == 3);
  check("4e fermee immediatement close(true)", g_dernierClose == true);
  check("4e ne compte pas dans actifs", tcp.actifs() == 3);
  check("compteur refuses = 1", tcp.refuses() == 1);
  check("cause = CONCURRENCE", tcp.refus() == GE_REFUS_CONCURRENCE);
  check("refus en 503", strstr(g_dernierEcrit, "503 Service Unavailable") != nullptr);
  check("refus avec Retry-After", strstr(g_dernierEcrit, "Retry-After: ") != nullptr);
  check("refus avec X-Guard-Reason: concurrence",
        strstr(g_dernierEcrit, "X-Guard-Reason: concurrence") != nullptr);
  check("refus avec Content-Length: 0", strstr(g_dernierEcrit, "Content-Length: 0") != nullptr);
  check("pic memorise", tcp.pic() == 3);

  // Les 3 clients admis restent vivants (le framework en prend possession).
  check("aucune fuite : 3 clients vivants", FauxTcpClient::vivants == 3);
  fauxTcpLiberer();
  check("arret du serveur : 0 client vivant", FauxTcpClient::vivants == 0);
}

// ── T39 : le compteur est rendu à la déconnexion ────────────────
// Protège : compteur jamais rendu → plus aucune connexion acceptée
// jusqu'au reboot (symptôme « 200 + corps vide » du Coulométre v5.30).
static void t39_deconnexion() {
  printf("\nT39 — compteur TCP rendu a la deconnexion\n");
  Guard_ESP g; reset(g);
  FauxTcpServeur srv;
  GE_TcpGuard<FauxTcpServeur, FauxTcpClient, FauxTcpRequest> tcp(g, srv, 2);
  tcp.installer();

  FauxTcpClient* c1 = new FauxTcpClient();
  srv.connecter(c1);
  check("1 connexion active", tcp.actifs() == 1);
  fauxTcpDeconnecter();
  check("compteur rendu : 0 actif", tcp.actifs() == 0);
  fauxTcpDeconnecter();   // déconnexion en trop (client qui disparaît)
  check("pas de sous-debordement (uint8_t) : 0 actif", tcp.actifs() == 0);

  srv.connecter(new FauxTcpClient());
  check("une nouvelle connexion est acceptee apres", tcp.actifs() == 1);
  fauxTcpDeconnecter();
  fauxTcpLiberer();
}

// ── T40 : échec d'allocation → abort + delete, compteur rendu ───
static void t40_echec_alloc() {
  printf("\nT40 — echec d'allocation : abort + delete, compteur rendu\n");
  Guard_ESP g; reset(g);
  FauxTcpServeur srv;
  GE_TcpGuard<FauxTcpServeur, FauxTcpClient, FauxTcpRequest> tcp(g, srv, 3);
  tcp.installer();

  g_dernierAbort = false;
  g_createEchoue = true;
  srv.connecter(new FauxTcpClient());   // détruit par le garde (abort)
  g_createEchoue = false;
  check("client avorte (abort)", g_dernierAbort == true);
  check("compteur rendu : 0 actif", tcp.actifs() == 0);
  check("echec d'allocation compte", tcp.echecsAlloc() == 1);

  srv.connecter(new FauxTcpClient());
  check("le service repart apres l'echec", tcp.actifs() == 1);
  fauxTcpDeconnecter();
  fauxTcpLiberer();
}

// ── T41 : garde mémoire = arrêt du service (priorité sur concurrence) ──
static void t41_garde_memoire() {
  printf("\nT41 — garde memoire : le service s'arrete au lieu de planter\n");
  Guard_ESP g; reset(g);
  FauxTcpServeur srv;
  GE_TcpGuard<FauxTcpServeur, FauxTcpClient, FauxTcpRequest> tcp(g, srv, 3);
  tcp.installer();

  g_fakeHeap = 2500;   // sous gardeHeap (3000, profil ESP8266)
  g_dernierEcrit[0] = '\0';
  srv.connecter(new FauxTcpClient());
  check("refus quand la RAM est insuffisante", tcp.refuses() == 1);
  check("cause = GARDE", tcp.refus() == GE_REFUS_GARDE);
  check("refusGarde compte", tcp.refusGarde() == 1);
  check("aucune connexion admise", tcp.actifs() == 0);
  check("X-Guard-Reason: garde", strstr(g_dernierEcrit, "X-Guard-Reason: garde") != nullptr);

  // Priorité : même saturé en concurrence, la cause affichée reste la mémoire.
  g_fakeHeap = 4700; g_fakeBloc = 3700;
  srv.connecter(new FauxTcpClient());
  srv.connecter(new FauxTcpClient());
  srv.connecter(new FauxTcpClient());
  check("3 connexions admises (RAM revenue)", tcp.actifs() == 3);
  g_fakeBloc = 500;    // sous gardeBloc (1200) : fragmentation
  srv.connecter(new FauxTcpClient());
  check("fragmentation -> GARDE, pas CONCURRENCE", tcp.refus() == GE_REFUS_GARDE);

  // Désactivation explicite : on repasse en refus de concurrence.
  g_fakeBloc = 3700;
  tcp.setGardeMemoire(false);
  g_fakeHeap = 1000;
  srv.connecter(new FauxTcpClient());
  check("garde desactivee : la RAM ne bloque plus", tcp.refus() == GE_REFUS_CONCURRENCE);
  g_fakeHeap = 4700;
  fauxTcpLiberer();
}

// ── T42 : contrat du refus (zéro allocation) et bornes ──────────
static void t42_contrat_tcp() {
  printf("\nT42 — contrat du refus TCP : zero allocation, bornes\n");
  Guard_ESP g; reset(g);
  FauxTcpServeur srv;
  GE_TcpGuard<FauxTcpServeur, FauxTcpClient, FauxTcpRequest> tcp(g, srv, 1);
  tcp.installer();

  g_creations = 0; g_dernierEcritLen = 0;
  srv.connecter(new FauxTcpClient());   // admise
  srv.connecter(new FauxTcpClient());   // refusée
  check("un refus ne cree aucune requete", g_creations == 1);
  check("le refus tient dans le tampon de pile",
        g_dernierEcritLen > 0 && g_dernierEcritLen < 192);

  tcp.setMaxConnexions(0);
  check("plafond 0 ramene a 1 (jamais de coupure totale)", tcp.maxConnexions() == 1);

  char buf[128];
  size_t n = tcp.resume(buf, sizeof(buf));
  check("resume() ecrit sans String", n > 0 && strstr(buf, "tcp actifs=") != nullptr);
  check("resume() respecte la borne len-1", n <= sizeof(buf) - 1);
  check("resume() sur tampon nul ne plante pas", tcp.resume(nullptr, 10) == 0);
  fauxTcpDeconnecter();
  fauxTcpLiberer();
}

// ── T43 : surcharge installer(Enregistreur) ─────────────────────
// Protège : AsyncWebServer n'expose PAS onClient (le handler vit sur son
// membre protégé `_server`). Sans cette surcharge, l'adaptateur ne peut pas
// être installé sur le serveur réel du Coulométre — défaut trouvé au build.
static void t43_installer_enregistreur() {
  printf("\nT43 — installer(Enregistreur) : serveur sans onClient public\n");
  Guard_ESP g; reset(g);
  FauxTcpServeur srv;
  GE_TcpGuard<FauxTcpServeur, FauxTcpClient, FauxTcpRequest> tcp(g, srv, 2);

  int appels = 0;
  // L'enregistreur reçoit le callback et le pose où il veut (ici : sur le
  // serveur, comme le fait LimitedWebServer::onClientConn()).
  tcp.installer([&srv, &appels](std::function<void(void*, FauxTcpClient*)> cb) {
    appels++;
    srv.onClient(cb, &srv);
  });
  check("l'enregistreur est appele une fois", appels == 1);
  check("le handler est bien pose", srv.installations == 1);

  srv.connecter(new FauxTcpClient());
  check("la connexion est admise via cet enregistreur", tcp.actifs() == 1);
  srv.connecter(new FauxTcpClient());
  srv.connecter(new FauxTcpClient());
  check("le plafond s'applique aussi", tcp.refuses() == 1);
  fauxTcpDeconnecter();
  fauxTcpLiberer();
}

int main() {
  printf("=== Tests Guard_ESP v%s ===\n", GE_VERSION);
  t1_fenetre();      t2_poids();        t3_pression();
  t4_concurrence();  t5_body();         t6_garde();
  t7_backoff();      t8_duree();        t9_inactivite();
  t10_calibrage();   t11_profils();     t12_routes();
  t13_resume();      t14_empreinte();   t15_double_rafale();
  t16_tickets();     t17_inactif();     t18_init();
  t19_sortir_orphelin(); t20_slots();   t21_tick();
  t22_xoff();        t23_resume_contrat(); t24_instance();
  test25_headers();  test26_async();    t27_reglages_actifs();
  t28_routes_chaine(); t29_formules();  t30_bornes();
  t31_print();       t32_exemple();     t33_credit();
  t34_compteurs();   t35_refus_texte();  t36_ticket_inactif();
  t37_tick_concurrence();
  t38_plafond_tcp(); t39_deconnexion(); t40_echec_alloc();
  t41_garde_memoire(); t42_contrat_tcp();
  t43_installer_enregistreur();
  printf("\n=== %d/%d tests OK ===\n", g_ok, g_total);
  return (g_ok == g_total) ? 0 : 1;
}
