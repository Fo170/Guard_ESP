# analyse.md — Recherche de bugs Guard_ESP (cycle 1, v1.0.0)

> Procédure `code-reviews-openfox` · Cadrage du 2026-10-22 · Cible : `Guard_ESP` v1.0.0

---

## 0. Cadrage

### Déclencheur
La librairie vient d'être créée (v1.0.0, commit `d4738fc`) et publiée. Avant toute
adoption sur le parc (12 IoT), elle doit passer une revue complète : c'est elle qui
sera chargée de protéger les appareils contre l'OOM, il serait absurde qu'elle en
introduise.

### Définition d'un « bug » retenue
Tout écart entre le comportement attendu et le comportement réel, sur 4 axes :

1. **Correction fonctionnelle** — la décision d'admission est-elle juste ?
2. **Sécurité mémoire** — la lib est-elle vraiment sans allocation ? (c'est sa promesse)
3. **Sûreté de l'API** — peut-on l'utiliser d'une façon qui la casse silencieusement ?
4. **Exactitude documentation ↔ code** — le README dit-il ce que fait le code ?

### Hors périmètre
- `Server_MCP` et `Crash_ESP_Monitor` : libs tierces, non modifiables dans ce cycle.
- **Les valeurs de seuils** (4 200 o, 25 000 o, 8 192 o…) : dérivées de mesures
  ponctuelles, à valider par campagne terrain — pas par relecture.
- `LICENSE`, `keywords.txt` : pas de logique.

### Périmètre fichiers
`src/` (4 headers, 861 l.) · `tests/` (3 fichiers, 284 l.) · `examples/` (2 projets)
· `README.md`, `LIVRE_BLANC.md`, `changelog.md`, `library.json`, `library.properties`.

### Vérifications déjà exécutées (état de départ)
- `g++ -std=c++17 -Wall -Wextra` → 0 warning, **47/47** tests, `sizeof = 232 o`.
- `pio run -e esp12e` et `-e esp32dev` → SUCCESS sur les 2 exemples.
- Variantes : `-DGE_ROUTES=0` OK ; `-DGE_BUCKETS=4/16/255` OK ; **`-DGE_BUCKETS=256` → boucle infinie** ; `-DGE_BUCKETS=1` → static_assert.

---

## 1. Faits établis (mesurés, pas supposés)

### F1 — `_bucket[]` n'est pas initialisé
`Guard_ESP.h:480` : `uint16_t _bucket[GE_BUCKETS];` sans initialiseur, alors que
`_routes[]` (518) est correctement initialisé à zéro. Sans appel à `demarrer()`,
`charge()` (439) lit des valeurs indéterminées.
**Reproduit** : pile polluée à `0xAA` → `charge = 65535` → **100 % des requêtes
refusées en FLUX**. Le README présente `demarrer()` comme « arme le cooldown
post-reboot », pas comme une initialisation obligatoire.

### F2 — `GE_AsyncWebGuard` ne compile pas
`Guard_ESP_web.h:125` : `typename RequestType::Response* resp = req->beginResponse(...)`.
La classe `AsyncWebServerRequest` n'a **aucun type imbriqué `Response`**, et
`beginResponse()` retourne `AsyncWebServerResponse*`. Le template n'est instancié
nulle part (aucun exemple, aucun test) → **code mort jamais compilé**.

### F3 — `admit()` retourne `true` sans prendre de slot
`Guard_ESP.h:219` : si `!_actif`, on incrémente `_admis` et on retourne `true` sans
toucher `_actifs` ni `_reqStart`. Le contrat « `true` = slot acquis, à rendre par
`sortir()` » est violé. Un `sortir()` ensuite calcule `millis() - _reqStart` sur une
valeur périmée.
**Mesuré** : `dureePire = 105 000 ms` pour une requête instantanée, puis refus
BACKOFF sur la requête suivante.

### F4 — Cycle `admit()`/`sortir()` déséquilibrable
- `sortir()` sans `admit()` : `_dureePire = millis()`, `_depassements++`, cooldown armé.
- `admit()` deux fois sans `sortir()` (possible avec `_concurrence = 3` sur ESP32) :
  **slots perdus définitivement**. Mesuré : `_actifs` reste à 2 après un seul `sortir()`.

### F5 — `_reqStart` unique pour N slots concurrents
`Guard_ESP.h:263/486` : un seul `uint32_t _reqStart` pour `_concurrence` requêtes.
Le 2ᵉ `admit` écrase la valeur → `sortir()` mesure depuis le dernier admit, pas
depuis celui de la requête rendue. La garde de durée devient **aveugle** dès
`_concurrence > 1` — c'est-à-dire sur le profil ESP32 par défaut.

### F6 — `tick()` et `sortir()` comptent deux fois le même dépassement
`tick()` (292) incrémente `_depassements` puis remet `_actifs = 0` ; le `sortir()`
du handler qui se termine enfin recompte (277). Mesuré : 1 → 2.

### F7 — `_xoff` périmé sur deux branches de refus
Les branches CONCURRENCE (244) et BODY (247-251) ne rafraîchissent pas `_xoff`.
**Mesuré** : heap 3 000 o (< XOFF 4 200) + refus CONCURRENCE → `xoff()` retourne
**false** alors que l'appareil est réellement sous pression.

### F8 — `GE_BUCKETS`/`GE_ROUTES` ≥ 256 : boucle infinie
`uint8_t garde = GE_BUCKETS` (424) tronque ; `for (uint8_t i = 0; i < GE_BUCKETS; i++)`
(184, 432, 439) devient infini dès 256. **Vérifié : `demarrer()` bloque (timeout).**
Aucun `static_assert` de borne haute.

### F9 — Trois membres morts, un compteur inatteignable
`_poidsMax` (462), `_heapRefus` (468), `_blocRefus` (469) sont écrits mais **jamais lus**.
`_rejDuree` (507) n'est jamais incrémenté : aucun `refuser(GE_REFUS_DUREE)` n'existe.
Conséquence : les « refus totaux » annoncés dans les profils (3 000/1 200 et
20 000/10 000) ne protègent **rien**, et la garde reste inactive tant que
l'application n'appelle pas `setGarde()` ou `autoCalibrer()`.

### F10 — `resume()` trompe l'appelant
`Guard_ESP.h:401-417` : `snprintf` retourne la longueur **qui aurait été écrite**.
Avec `buf[20]`, la fonction retourne **196** alors que 19 caractères sont écrits,
ce qui contredit le commentaire (397-398). De plus `resume()` lit `_bucket[_idx]`
sans vieillir la fenêtre → après une longue inactivité il affiche un seau périmé,
alors que `chargeCourante()` (365) vieillit la fenêtre : les deux voies d'affichage
divergent.

### F11 — `GE_PressionCb` observe une autre instance que l'adaptateur
`Guard_ESP_mcp.h:24` : `GE_PressionCb()` appelle `guardInstance()`, alors que
`GE_McpFlow` porte une référence `_g` (29). L'exemple 02 utilise `Guard_ESP guard;`
+ `GE_McpFlow<Server_MCP> flux(guard);` → le callback observe une **seconde
instance** non configurée (profil AUTO → XOFF 25 000 o), distincte de celle qui décide.

### F12 — `Guard_ESP_web.h` alloue 4 `String` par refus
`web.h:79-86` et `128-135`. Contredit la promesse « zéro allocation dynamique »
(README:10, `library.json`, `Guard_ESP.h:18`) et applique l'anti-patron M7 du livre
blanc **au moment le plus critique** (pression mémoire).

### F13 — `route()` stocke un pointeur non copié
`Guard_ESP.h:303-320` : un appelant passant un buffer local ou `String::c_str()`
laisse un pointeur pendant. Table saturée (`> GE_ROUTES` noms) → `_routeCur = -1`
et les compteurs sont **silencieusement ignorés**.

### F14 — Réentrance revendiquée mais non garantie
`Guard_ESP.h:21` annonce « réentrant : appelable depuis `loop()` et depuis un handler
web ». Aucun verrou, aucun atomique. Sur ESP32 + AsyncWebServer, les callbacks
s'exécutent dans la tâche `async_tcp` en parallèle de `loop()` → courses sur
`_actifs`, `_bucket`, compteurs.

### F15 — 10 écarts documentation ↔ code
| # | Document | Affirmation | Réalité |
|---|---|---|---|
| 1 | README:25, `Guard_ESP.h:188` | réutilise `Crash_ESP_Monitor` via `__has_include` | **aucun `__has_include`** dans le projet |
| 2 | LIVRE_BLANC:23 | API `guard.reject()` | méthode inexistante |
| 3 | LIVRE_BLANC:3,32,309 ; README:44 | « 34 problèmes » | les tableaux en contiennent **49** |
| 4 | LIVRE_BLANC:243 vs `autoCalibrer`:175 | « bloc mini = H_ref × 0,35 » | garde = 0,4 × (0,35·H) = **0,14·H_ref** |
| 5 | LIVRE_BLANC:253 | Relais 20 300 o → **6** canaux | `href > 20000 → 8` → **8** canaux |
| 6 | LIVRE_BLANC:284 | « compteur de démarrages courts » | inexistant (c'est `CM_BOOT_COURT_MS`) |
| 7 | README:82 | `tick()` = « watchdog d'inactivité » | `tick()` libère les slots, ne touche pas `_derniereActivite` |
| 8 | `print.h:23` | compatible **AsyncWebServer** | `AsyncWebServer` n'a pas `sendContent()` → ne compile pas |
| 9 | README:206 / LIVRE_BLANC:217 | concurrence ESP32 « 2–4 » | valeur **fixe 3** |
| 10 | README table API | 13 méthodes listées | 15 méthodes publiques omises |

---

## 2. Cartographie du gaspillage / de l'incohérence

### 2.1 Membres jamais lus (BSS inutile)
`_poidsMax` (2 o), `_heapRefus` (4 o), `_blocRefus` (4 o) → **10 octets** sur 232.

### 2.2 Membre jamais incrémenté
`_rejDuree` (4 o) + les 8 messages associés (`refusTexte`, `ge_messageRefus`) → code mort.

### 2.3 État non réinitialisé par `demarrer()`
`_actifs`, `_reqStart`, `_refus`, tous les compteurs. Vérifié : `_actifs = 2` survit
à `demarrer()`.

### 2.4 Clamps incohérents dans `regler()` (129-142)
`canaux → 1` (au lieu de `GE_CANAUX_DEF = 4`), `partHeap → 10` (au lieu de
`GE_PART_HEAP_DEF = 25`), `fenetreMs → GE_FENETRE_DEF`. Trois replis, trois
politiques différentes. `_partHeap` non borné (> 100 accepté → plafond décoratif).

---

## 3. Points d'entrée et commandes

```bash
# Tests hôte
cd Guard_ESP
g++ -std=c++17 -Wall -Wextra -I tests -I src tests/test_guard.cpp -o /tmp/t && /tmp/t

# Variantes de configuration
g++ -std=c++17 -DGE_BUCKETS=256 -I tests -I src tests/test_guard.cpp -o /tmp/t256   # boucle infinie
g++ -std=c++17 -DGE_ROUTES=0     -I tests -I src tests/test_guard.cpp -o /tmp/t0     # OK

# Compilation croisée
cd examples/01_web_basic && pio run -e esp12e && pio run -e esp32dev
cd examples/02_mcp_basic && pio run -e esp12e && pio run -e esp32dev
```

---

## 4. Invariants à préserver (non-régression)

1. **`sizeof(Guard_ESP) < 300 o`** — critère d'acceptation de la v1.0.0.
2. **Zéro `malloc`/`new`/`String` dans le noyau** `Guard_ESP.h`.
3. **47 tests hôte doivent rester verts** (puis croître avec les nouveaux cas).
4. **`pio run` SUCCESS** sur `esp12e` et `esp32dev`, 0 warning `-Wall -Wextra`.
5. **Compatibilité C++11** (ESP32) : pas de CTAD, pas de `std::optional`.
6. **API publique rétrocompatible** : `admit(poids, taille)`, `sortir()`, `resume(buf, len)`.
7. **`Crash_ESP_Monitor` et `Server_MCP` non modifiés.**
8. **Les 3 profils et leurs valeurs numériques** restent identiques (sauf correction
   documentaire justifiée).

---

## 5. État d'avancement des passes — CYCLE 1 CLOS

| Passe | Contenu | Statut |
|---|---|---|
| 0 | Cadrage | ✅ |
| 1 | Analyse (ce document) | ✅ |
| 2 | Relecture `code_reviewer` | ✅ 5 critiques · 17 avertissements · 7 suggestions · 4 info |
| 2.5 | Triage | ✅ 15/15 faits confirmés, 5 critiques ajoutés |
| 3 | Plan + options | ✅ périmètre A |
| 3.5 | `criteres.md` | ✅ 26 critères + 7 de non-régression |
| 4 | Correctifs | ✅ cycle 1 (26 findings) + cycle 2 (12 findings) |
| 5 | Vérification | ✅ 3 échecs trouvés (W8, W17, C3 partiel) puis corrigés |
| 6 | Re-relecture | ✅ 2 critiques + 9 avertissements introduits par les correctifs, puis corrigés |
| 7 | Compactage | ✅ ce document |
| 8 | Capitalisation | ✅ `reference/patterns.md` + bump du skill |

---

## 6. Bilan consolidé (état final v1.1.0)

### Ce qui a été corrigé

| Catégorie | Nombre | Détail |
|---|---|---|
| Critiques (cycle 1) | 5 | C1 fenêtre 2× quota · C2 fuite slot · C3 thread-safety · C4 `!actif` · C5 `_bucket` non initialisé |
| Avertissements (cycle 1) | 17 | W1–W17 |
| Critiques (cycle 2, introduits par les correctifs) | 3 | 28 ticket fictif falsy · 29 compteur fantôme global · 30 `tick()` contourne le plafond |
| Avertissements (cycle 2) | 9 | 31–39, 41, 48 |
| **Total corrigé** | **34** | |
| Reportés (justifiés) | 3 | copie de `Guard_ESP`, désarmement `GE_McpFlow`, divergence d'en-têtes sync/async |

### Preuves finales

```
tests hôte        : 149/149 (47 → 137 → 149)
sizeof            : 256 octets (< 300, static_assert)
allocation noyau  : aucune
bornes            : -DGE_BUCKETS=256 / GE_ROUTES=128 / GE_SLOTS=32 / GE_PRINT_BUF=4 → rejet compilatoire
builds            : esp12e + esp32dev SUCCESS × 2 exemples, 0 warning Guard_ESP
versions          : header = json = properties = README = changelog = 1.1.0
libs tierces      : Server_MCP inchangé, Crash_ESP_Monitor inchangé (modifs du 17/10)
```

### Leçon principale du cycle

Les correctifs d'un garde-fou mémoire sont eux-mêmes des sources de bugs. Les
trois défauts les plus graves du cycle 2 (ticket falsy, compteur fantôme,
`tick()` qui remet à zéro) ont **tous** été introduits par des correctifs du
cycle 1. D'où la règle retenue : **l'état d'une requête doit être porté par la
requête (ticket), jamais par un compteur partagé du garde.**
