# Changelog — Guard_ESP

Toutes les évolutions notables de la librairie.
Format : une section par version, avec les vérifications associées.

---

## 2026-10-29 — v1.2.0 (plafond de connexions TCP)

Ajout d'un **troisième niveau de protection** : `Guard_ESP_tcp.h`.

### Pourquoi

`Guard_ESP` (noyau) et `Guard_ESP_web` décident **dans le handler**, donc
**après** que le framework a créé un `AsyncWebServerRequest`. Or c'est
précisément cette création qui tue un ESP8266 sous pression : mesuré le
02/10/2026 sur le projet Coulométre — allocation de 32 o refusée dans
`AsyncWebServerRequest::_parseReqHeader()` → `std::bad_alloc` → panic →
**reboot loop toutes les 5-20 s**. Un refus qui a lieu dans le handler est
déjà trop tard : la requête est allouée.

`GE_TcpGuard` décide dans `onClient()`, donc **avant toute allocation** :
une connexion refusée ici ne crée aucun objet.

### Nouveautés

| Élément | Rôle |
|---|---|
| `GE_TcpGuard<ServerType, ClientType, RequestType>` | Plafond de connexions simultanées + garde mémoire, décidés au niveau TCP |
| `installer()` | Remplace le `onClient` du framework (à appeler **après** le constructeur du serveur) |
| `installer(Enregistreur)` | Variante pour un serveur qui n'expose pas `onClient` (`AsyncWebServer` : le handler est sur `_server`, protégé) — l'appelant fournit l'enregistreur |
| `setGardeMemoire(bool)` | **Arrêt du service** quand `Guard_ESP::gardePassee()` est faux (RAM ou plus gros bloc contigu insuffisant) — activé par défaut |
| `setMaxConnexions(n)` | Plafond ; `0` est ramené à `1` (jamais de coupure totale, l'OTA reste joignable) |
| `resume(buf, len)` | Diagnostic sans `String` (même convention que le noyau) |
| `__GUARD_ESP_H_VERSION__` | Macro de version (convention Fo170) — `GE_VERSION` en dérive ; lue par `liste_src` des projets |

### Invariants respectés

- **Zéro allocation sur le chemin de refus** : l'en-tête 503 est construit
  dans un tampon de **pile** (`snprintf`, 192 o). Aucune `String`, aucun
  `new` — ce chemin doit réussir même à 200 o de heap.
- Comportement d'origine **reproduit** à l'admission :
  `setRxTimeout(3)` puis `RequestType::create()`, et `abort()` + `delete`
  si la création échoue.
- Sur refus : `close(true)` (**immédiat**) puis `delete`.
  `close(false)` serait différé → **use-after-free**.
- Compteur rendu sur **tous** les chemins : échec d'allocation **et**
  déconnexion (`onDisconnect`). Un compteur jamais rendu interdit toute
  nouvelle connexion jusqu'au reboot.
- **Aucun `#include` du framework** dans ce header : les types sont des
  paramètres de gabarit → pas de conflit d'enum `HTTPMethod`, et test
  possible sur l'hôte avec de faux serveurs.
- Ordre des refus : **garde mémoire AVANT concurrence**. Sous pression, la
  cause affichée doit être la mémoire — sinon on cherche au mauvais endroit.

### Vérifications

`192/192` vérifications hôte, `0` échec, `0` warning (`-Wall -Wextra`),
`AddressSanitizer` + `UBSan` propres (aucune fuite, aucun `free` invalide).

| Test | Ce qu'il verrouille |
|---|---|
| `T38` | 4ᵉ connexion refusée : **aucune requête créée**, `close(true)`, 503 + `Retry-After` + `X-Guard-Reason`, pic mémorisé |
| `T39` | Compteur rendu à la déconnexion ; pas de sous-débordement `uint8_t` sur double déconnexion |
| `T40` | Échec d'allocation → `abort()` + `delete`, compteur rendu, le service repart |
| `T41` | RAM insuffisante → `GE_REFUS_GARDE` ; **priorité sur la concurrence** ; fragmentation seule suffit ; désactivation possible |
| `T42` | Un refus ne crée aucune requête ; refus < 192 o ; plafond `0` → `1` ; `resume()` borné |
| `T43` | Surcharge `installer(Enregistreur)` : `AsyncWebServer` n'expose pas `onClient` (le handler vit sur son membre protégé `_server`) — sans elle, l'adaptateur ne peut PAS être installé sur un serveur réel (défaut trouvé au build du Coulométre) |
| `T43` | Surcharge `installer(Enregistreur)` : `AsyncWebServer` n'expose pas `onClient` (handler sur `_server`, protégé) — sans elle, l'adaptateur ne peut pas être installé sur un serveur réel |

---

## 2026-10-22 — v1.1.0 (cycle de chasse aux bugs n°1)

Premier cycle `code-reviews-openfox`. **5 findings critiques** et **17
avertissements** corrigés, dont trois défauts qui annulaient partiellement la
promesse de la v1.0.0.

### Critiques

| # | Finding | Cause | Correctif | Preuve |
|---|---|---|---|---|
| C1 | Fenêtre glissante laissant passer **2× le quota** : 8 requêtes admises en 26 254 ms (quota 4/30 000) | Un seau était remis à zéro `GE_BUCKETS × bucketMs` après son **début**, donc un événement du début du seau n'était compté que 7/8 de fenêtre | Anneau de **`GE_BUCKETS + 1`** seaux : un seau n'est purgé qu'après `(GE_BUCKETS+1)` rotations, donc tout événement reste compté **au moins** `_fenetreMs` | Test `T15` : 4 admis à `+3 749 ms`, 4 à `+30 003 ms` → **0 admis** au 2ᵉ lot |
| C2 | Fuite de slot de concurrence : `actifs=1` après 2 `admit()` + 2 `fin()` | L'état « ouvert » était un `bool` **par instance d'adaptateur**, écrasé par toute requête concurrente | **`GE_Ticket`** (RAII) : le jeton porte **son** index de slot, la libération est exacte à la destruction | Test `T16` : 2 tickets → `actifs=0` après destruction |
| C3 | Noyau non thread-safe alors que l'en-tête annonçait « réentrant » | Aucun verrou ; sur ESP32 + AsyncWebServer les callbacks tournent dans `async_tcp`, en parallèle de `loop()` | Verrou **`portMUX`** (ESP32) / sans objet (ESP8266) sur `admitInterne`, `sortir`, `tick`, `route`, `routeOk`, `routeKo` ; mention « réentrant » **supprimée** | `grep -ci reentrant` = 0 ; `GE_VERROU` présent dans 6 fonctions |
| C4 | `admit()` retournait `true` sans slot quand `!_actif` → `dureePire = 105 000 ms` | Contrat « true = slot acquis » violé | Compteur **`_slotsSansMesure`** : `sortir()` le consomme sans mesurer de durée | Test `T17` : `dureePire=0`, `depassements=0` |
| C5 | `_bucket[]` non initialisé → **100 % de refus FLUX** sans `demarrer()` | Tableau sans initialiseur (contrairement à `_routes[]`) | `uint16_t _bucket[GE_BUCKETS+1] = {}` | Test `T18` : `admit()` fonctionne sans `demarrer()` |

### Avertissements

| # | Finding | Correctif | Preuve |
|---|---|---|---|
| W1 | `sortir()` sans `admit()` fabriquait une durée (`dureePire = millis()`) | Sortie anticipée si le slot n'est pas marqué occupé | `T19` |
| W2 | Un seul `_reqStart` pour N slots → garde de durée aveugle dès `_concurrence > 1` | Tableau **`_slotStart[GE_SLOTS]`** + `_slotBusy[]` : un temps de départ par slot | `T20` |
| W3 | `tick()` puis `sortir()` comptaient deux fois le dépassement | `tick()` marque les slots libres ; `sortir()` sort tôt si le slot n'est plus occupé | `T21` |
| W4 | `_xoff` périmé sur BACKOFF / GARDE / CONCURRENCE / BODY | Un seul échantillonnage mémoire en tête d'`admit()`, publié sur **toutes** les sorties | `T22` |
| W5 | `GE_BUCKETS`/`GE_ROUTES` ≥ 256 → **boucle infinie** (`for (uint8_t i…)`) ; `int8_t _routeCur` perdait les routes ≥ 128 | `static_assert` de bornes hautes + compteurs `uint16_t` | `-DGE_BUCKETS=256` → **échec de compilation** |
| W6 | `resume()` retournait 191 pour `buf[20]` et affichait le seau courant au lieu de la charge | Retour `min(n, len-1)` ; charge = somme de l'anneau | `T23` |
| W7 | `GE_PressionCb()` observait `guardInstance()` au lieu de l'instance de l'adaptateur → sur ESP8266, XOFF permanent | `GE_InstancePression()` armé par `appliquer()` ; surcharge `GE_PressionCb(Guard_ESP*)` | `T24` |
| W8 | ~9 `String` allouées par refus web, **au pire moment** | En-têtes émis **uniquement hors pression** (`!_g.enPression()`) | `T25` : 36 en-têtes hors pression, **0** sous pression |
| W9 | `GE_AsyncWebGuard` **ne compilait pas** (`typename RequestType::Response` inexistant) | `auto*` sur le retour de `beginResponse()` | `T26` compile et refuse |
| W10 | `_poidsMax`, `_heapRefus`, `_blocRefus` écrits mais **jamais lus** | `_poidsMax` appliqué dans `admit()` ; `profil()` arme la garde avec les seuils « refus total » | `T27` |
| W11 | `route()` comparait les **pointeurs** → routes distinctes confondues | Comparaison par `strcmp` uniquement + drapeau `routesSaturees()` | `T28` |
| W12 | `autoCalibrer()` écrasait `regler()` et la garde valait 0,14·H au lieu de 0,35·H | Les 4 formules documentées appliquées exactement : 0,75 / 0,35 / 0,55 / 0,25 × H_ref | `T29` |
| W13 | `regler()` tronquait `concurrence` (300 → 44) et ne bornait pas `partHeap` | Validation complète ; retour `false` et **configuration conservée** | `T30` |
| W14 | Destructeurs de `GE_ServerPrint`/`GE_ChunkPrint` ne vidaient pas le tampon ; `write()` masquait l'échec | Destructeurs → `vider()` ; `GE_ChunkPrint::write()` retourne 0 si la cible refuse | `T31` |
| W15 | `GE_REFUS_DUREE` inatteignable (aucun `refuser()` ne l'émettait) | Cause **supprimée** ; remplacée par `GE_REFUS_INACTIF` | `grep -c GE_REFUS_DUREE` = 0 |
| W16 | Exemple 01 refusé en `BODY` sur 4,7 Ko (`taille=1200` > 25 % du heap) | `taille` = **plus gros bloc alloué** (192 o), sémantique documentée | `T32` + build |
| W17 | 10 écarts documentation ↔ code | Tous corrigés (voir plus bas) | Revue + `grep` |

### Suggestions retenues

| # | Finding | Correctif |
|---|---|---|
| S1 | heap/bloc interrogés jusqu'à 5× par décision | **1 seul** échantillonnage par `admit()` |
| S2 | `regler()` changeait `_bucketMs` sans invalider la fenêtre | `regler()` vide la fenêtre |
| S3 | `sizeof < 300` vérifié à l'exécution | `static_assert(sizeof(Guard_ESP) < 300)` |
| S4 | `X-RateLimit-Remaining` manquant | En-tête ajouté + `creditRestant()` |

### Documentation corrigée (W17)

| Écart | Correction |
|---|---|
| « réutilise `Crash_ESP_Monitor` via `__has_include` » | Le code est **totalement autonome** ; aucun `__has_include` |
| `guard.reject()` dans le livre blanc | Remplacé par `guard.ticket()` |
| « 34 problèmes » | **49** (comptage réel des tableaux) |
| Formules d'`autoCalibrer()` (garde 0,35·H) | 0,75 / 0,35 / 0,55 / 0,25 × H_ref, explicitées |
| Relais 20 300 o → « 6 canaux » | **8** canaux (le code donne 8) |
| `tick()` présenté comme « watchdog d'inactivité » | Libère les slots bloqués ; le watchdog est `inactif()` |
| `GE_ServerPrint` « compatible AsyncWebServer » | **Non** : `AsyncWebServer` n'expose pas `sendContent()` |
| Concurrence ESP32 « 2–4 » | **3** (valeur réelle du profil) |
| Table API partielle | 15 méthodes ajoutées, dont `heapXoff()`, `creditRestant()`, `routesSaturees()` |
| `GE_REFUS_DUREE` documenté | Retiré (cause supprimée) |

### Cycle 2 (relecture de clôture) — 2 critiques introduits par les correctifs

La relecture de clôture a trouvé que deux correctifs du cycle 1 avaient
eux-mêmes introduit des défauts. Corrigés dans la même version.

| # | Finding | Cause | Correctif | Preuve |
|---|---|---|---|---|
| 28 | `GE_Ticket` d'un garde inactif **falsy** : le jeton était construit avec `idx = AUCUN` alors que `_slotsSansMesure` avait été incrémenté → compteur fantôme jamais décrémenté, et `GE_WebGuard` répondait 503 « protection désactivée » à une requête **admise** | L'état fictif était porté par un compteur global, pas par le slot | **`_slotFictif[GE_SLOTS]`** : un slot fictif est réellement alloué (ticket vrai, libération symétrique) mais sa durée n'est pas mesurée. `_slotsSansMesure` supprimé | `T36` : ticket vrai, `actifs=0` après destruction, `dureePire=0` |
| 29 | `_slotsSansMesure` global : si `_actif` bascule entre `admit()` et `sortir()`, la libération est attribuée à la **mauvaise** requête → slot fuyé | Compteur global au lieu d'un état par slot | idem (état porté par le slot) | `T36` : bascule `_actif` → 2 slots, libérations correctes |
| 30 | `tick()` remettait `_actifs = 0` au lieu de décrémenter → **plafond de concurrence contourné** (4 slots simultanés pour `concurrence()==3`) | Remise à zéro globale | Décrément par slot réellement libéré | `T37` : 3 slots max, 4ᵉ refusé |
| 31 | `refuser()` était appelé **hors verrou** → compteurs perdus sous concurrence | Sorties d'`admitInterne` après `GE_DEVERROU` | `refuserSousVerrou()` : la cause et les compteurs sont écrits **verrou tenu**, la fonction rend le verrou | revue : 1 entrée / 1 sortie par chemin |
| 32 | `chargeCourante()`/`creditRestant()`/`resume()` sont **mutants** (appellent `avancer()`) et n'étaient pas verrouillés | Accesseurs présentés comme de simples getters | Verrouillés et documentés comme mutants | commentaire d'en-tête + README |
| 33 | `profil()`/`autoCalibrer()` ignoraient l'échec de `regler()` → garde armée incohérente avec le XOFF (motif de l'incident du 01/09) | Retour `bool` non testé | `borneConcurrence()` ramène la valeur dans `[1, GE_SLOTS]` ; `autoCalibrer()` n'arme la garde que si `regler()` a réussi | `T29`, `T11` |
| 34 | Piège de migration muet : `if (!wguard.admit(...)) return;` compile et rend le slot **avant** le traitement | Ticket détruit en fin d'instruction | `GE_NODISCARD` (`warn_unused_result`) sur `admit()`/`ticket()` + README corrigé | warning à la compilation |
| 35 | README montrait l'API supprimée (`fin()`), omettait `X-RateLimit-Remaining`, annonçait 232 octets | Doc non migrée | README réécrit (ticket, 256 octets, en-têtes conditionnels, accesseurs mutants) | `grep -c "fin()"` = 0 |
| 36 | `GE_ServerPrint::write()` retournait toujours `n` (échec silencieux) | Retour de `sendContent()` ignoré | `_erreur` positionné si `sendContent()` écrit moins que demandé | `T31` |
| 37 | `resume()` sommait `_bucket[]` sans vieillir la fenêtre → « charge 4/4 » après 10 min d'inactivité | Fonction `const` | `resume()` devient **mutant** et appelle `avancer()` | `T13` |
| 39 | `retryAfterMs()` = 30 000 ms alors qu'un événement reste compté 33 750 ms → le client qui respecte l'en-tête est refusé une 2ᵉ fois | Anneau de 9 seaux non pris en compte | `retryAfterMs()` = `GE_RING × _bucketMs` | `T1` |
| 41 | `heapLibre()`/`blocLibre()` appelés **dans** la section critique | Échantillonnage après verrou | Échantillonnage **avant** `GE_VERROU` | revue |
| 48 | `GE_SLOTS` validé jusqu'à 16 alors que `sizeof` échoue dès 14 | Borne incohérente avec le `static_assert` d'empreinte | Borne ramenée à **13** | `-DGE_SLOTS=32` rejeté |

Reportés (hors périmètre de ce cycle, consignés) :
- `Guard_ESP` reste copiable alors qu'il contient un `portMUX` et que des
  tickets pointent dessus — à traiter si un usage le justifie (aucun aujourd'hui).
- `GE_McpFlow` n'arme pas de désarmement du pointeur d'instance.
- Politiques d'en-têtes divergentes entre adaptateurs sync et async.

### Changement d'API (rupture assumée)

`GE_WebGuard::admit()` et `GE_AsyncWebGuard::admit()` retournent désormais un
**`GE_Ticket`** au lieu d'un `bool`, et `fin()` disparaît. Motif : l'ancien
couple stockait l'état ouvert dans un booléen par instance, ce qui perdait des
slots (finding C2). Aucun consommateur n'existait (lib publiée la veille).
`Guard_ESP::admit()`, `sortir()` et `resume()` sont **inchangés**.

### Vérifications

- Tests hôte : **149/149** (47 → 137 → 149), `-Wall -Wextra` **0 warning**.
- `sizeof(Guard_ESP)` = **256 octets** (< 300, garanti par `static_assert`).
- Aucune allocation dynamique dans `Guard_ESP.h` (hors commentaires).
- `pio run` → **SUCCESS** sur `esp12e` et `esp32dev` pour les 2 exemples.
- Bornes de compilation : `-DGE_BUCKETS=256`, `-DGE_ROUTES=128`,
  `-DGE_SLOTS=32`, `-DGE_PRINT_BUF=4` → **échec de compilation** (voulu).
- `Crash_ESP_Monitor` et `Server_MCP` : **non modifiés**.

---

## 2026-10-22 — v1.0.0 (création)

Première version. Noyau de protection + trois adaptateurs, extraits des
protections déjà éprouvées sur le parc (Coulometre, ESP32-Telegram) et
généralisés pour combler les angles morts identifiés.

### Mécanismes

| Mécanisme | Problème couvert | Référence |
|---|---|---|
| Fenêtre glissante **pondérée** + `Retry-After` + `X-RateLimit-*` | rafale d'appels MCP | panic n°4 du 31/08/2026 |
| **Filtre de TOUTES les routes web** (`Guard_ESP_web.h`) | 429 contourné par `/` et le handshake | angle mort mesuré |
| XON/XOFF sur **heap ET plus gros bloc contigu** | OOM, panic par fragmentation | 31/08/2026 · recalibrage 35→30 Ko du 01/09 |
| Plafond de taille **avant** allocation (absolu + part du heap) | `MCP_MAX_BODY 8192` sur 4,7 Ko de heap | — |
| Plafond de concurrence (503) | `ESP8266WebServer` mono-client | gel 10 s sur Surveillance Batterie |
| Garde de durée par requête + backoff | WDT reset | raison n°7 sur les 2 ESP32 |
| Garde de démarrage `H_ref` | MCP démarré à 4,3 Ko | — |
| Watchdog d'inactivité | poussée Domoticz muette 9 h 34 | Coulometre F-19 |
| Compteurs par route (ok/ko) | `HTTP 200` + corps vide | Coulometre S1/F-20 |
| `GE_ServerPrint` / `GE_ChunkPrint` (buffer fixe 192 o) | réponses tronquées 0/949/1072 o | LinkysTIC · Coulometre |

### Profils

| Paramètre | ESP8266 | ESP32 |
|---|---|---|
| Concurrence | 1 | 3 |
| Canaux / fenêtre | 4 / 30 s | 8 / 30 s |
| XOFF heap / bloc | 4 200 / 1 800 | 25 000 / 12 000 |
| Refus total | 3 000 / 1 200 | 20 000 / 10 000 |
| Body max | 2 048 | 8 192 |
| Durée max requête | 800 ms | 2 500 ms |

Profils réellement distincts : l'ESP32 dispose de **13 à 18× plus de mémoire
contiguë** utilisable (mesuré : 3 442–3 897 o sur ESP8266 contre
44 843–68 972 o sur ESP32).

### Fichiers

- `src/Guard_ESP.h` — noyau (539 l.)
- `src/Guard_ESP_web.h` — adaptateurs synchrone et asynchrone (153 l.)
- `src/Guard_ESP_mcp.h` — adaptateur `Server_MCP` (54 l.)
- `src/Guard_ESP_print.h` — `Print` streamé sans `String` (115 l.)
- `examples/01_web_basic`, `examples/02_mcp_basic`
- `tests/` — 47 vérifications hôte + shim Arduino
- `LIVRE_BLANC.md` — taxonomie 8 familles / 34 problèmes, 12 pratiques, valeurs typiques

### Vérifications

- `sizeof(Guard_ESP)` = **232 octets** (< 300 requis).
- **Aucune allocation dynamique** dans le noyau (ni `malloc`, ni `new`, ni `String`).
- `pio run -e esp12e` → **SUCCESS** ; `pio run -e esp32dev` → **SUCCESS**, sur les 2 exemples.
- `-Wall -Wextra` : **0 warning** provenant de Guard_ESP.
- Tests hôte : **47/47**.
- `Crash_ESP_Monitor` **non modifié** — dépendance optionnelle à sens unique.

### Conception

- `Guard_ESP` **protège** (décide) ; `Crash_ESP_Monitor` **observe** (mesure).
- La mesure mémoire est empruntée à `Crash_ESP_Monitor` quand il est présent
  (`__has_include`), sinon repli sur les API natives.
- Aucun couplage forcé : les adaptateurs `web` et `mcp` sont des templates,
  donc les headers ne dépendent pas de `Server_MCP` ni de `ESPAsyncWebServer`.
