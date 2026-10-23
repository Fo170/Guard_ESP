# criteres.md — Critères d'acceptation du cycle 1

> Dérivés du triage de `analyse.md` et du rapport `code_reviewer`.
> Un finding n'est clos que si son critère est **vérifié par une commande**.

---

## Périmètre retenu : **A** (critiques + avertissements)

Reportés hors périmètre (justifiés) :
- **R-A1** — granularité fine de la fenêtre (< 3,75 s) : la granularité reste
  grossière par conception ; le correctif C1 garantit seulement qu'elle ne
  laisse plus passer le double du quota.
- **R-A2** — validation terrain des seuils (4 200 o, 25 000 o…) : nécessite une
  campagne 24 h sur matériel, hors relecture.
- **R-A3** — `_winStart == 0` comme sentinelle : devient sans effet après C5
  (`_bucket = {}`), consigné en ℹ️.
- **R-A4** — `ge_codeHttp(GE_REFUS_POIDS) = 429` : un refus structurel inviterait
  à réessayer ; corrigé en 503 car c'est un refus permanent.

---

## Critiques (5)

| ID | Finding | Critère de clôture | Méthode de vérification |
|---|---|---|---|
| **C1** | Fenêtre glissante : 8 requêtes admises en 26 254 ms (quota 4/30 000) | Sur un ring de `GE_BUCKETS+1` seaux, **aucune** fenêtre glissante de `_fenetreMs` ne contient plus de `_canaux` poids | Test `T15` : 4 admits à `t=+3749`, 4 à `t=+30003` → les 4 du 2ᵉ lot sont **refusés** |
| **C2** | `_ouvert` par instance → fuite de slot web (mesuré `actifs=1` après 2 admits + 2 fins) | Deux tickets simultanés libèrent **2 slots** ; l'API n'expose plus d'état partagé | Test `T16` : 2 tickets + destruction des 2 → `actifs()==0` |
| **C3** | Noyau non thread-safe alors qu'il s'annonce « réentrant » | (a) la doc ne prétend plus être réentrante ; (b) toute mutation d'état est sous verrou `portMUX` sur ESP32 | `grep -c "reentrant"` = 0 dans le header ; présence de `GE_VERROU` dans `admit/sortir/tick/route` |
| **C4** | `admit()` renvoie `true` sans slot quand `!_actif` → `dureePire=105 000 ms` | `!_actif` → `true` **sans** polluer `dureePire` ni `depassements` | Test `T17` : `actif(false)`, admit, sortir → `dureePire()==0` et `depassements()==0` |
| **C5** | `_bucket[]` non initialisé → 100 % de refus FLUX sans `demarrer()` | Un `Guard_ESP` fraîchement construit admet sans appel à `demarrer()` | Test `T18` : `Guard_ESP g; g.profil(ESP8266); g.admit(1)` → `true` (sans `demarrer()`) |

## Avertissements (17)

| ID | Finding | Critère | Méthode |
|---|---|---|---|
| **W1** | `sortir()` sans `admit()` → `dureePire = millis()` | `sortir()` sans slot ouvert ne modifie **aucun** compteur | Test `T19` : `sortir()` ×2 → `dureePire()==0`, `depassements()==0` |
| **W2** | `_reqStart` unique pour N slots → garde de durée aveugle | Chaque slot a **son** temps de départ ; la durée mesurée est celle du slot rendu | Test `T20` : 2 slots, durées distinctes → `dureePire()` = la plus grande |
| **W3** | `tick()` + `sortir()` comptent deux fois | Après `tick()`, le `sortir()` du handler ne recompte pas | Test `T21` : `tick()` puis `sortir()` → `depassements()==1` |
| **W4** | `_xoff` périmé sur BACKOFF/GARDE/CONCURRENCE/BODY | `xoff()` reflète l'état mémoire sur **toutes** les sorties de `admit()` | Test `T22` : heap bas + refus BODY → `xoff()==true` |
| **W5** | `GE_BUCKETS`/`GE_ROUTES` ≥ 256 → boucle infinie ; ≥ 128 routes perdues | Bornes hautes refusées **à la compilation** | `g++ -DGE_BUCKETS=256` → **échec de compilation** (static_assert) |
| **W6** | `resume()` retourne 191 pour `buf[20]` et affiche le seau, pas la charge | Retour ≤ `len-1` ; « charge » = charge de la fenêtre | Test `T23` : `resume(buf,20)` → retour ≤ 19 et texte tronqué proprement |
| **W7** | `GE_PressionCb` observe `guardInstance()` ≠ instance de l'adaptateur | Le callback de pression observe **l'instance de l'adaptateur** | Test `T24` : 2 instances, pression sur la 2ᵉ → le callback reflète la 2ᵉ |
| **W8** | `Guard_ESP_web.h` : ~9 `String` par refus, au pire moment | Sous pression mémoire (< 2×XOFF), **aucun** en-tête n'est construit | `grep` : les `sendHeader` sont dans un `if` de marge mémoire ; test `T25` |
| **W9** | `GE_AsyncWebGuard` ne compile pas | Compile avec un faux `AsyncWebServerRequest` | Test `T26` : compilation + refus |
| **W10** | `_poidsMax`, `_heapRefus`, `_blocRefus` jamais lus | Les trois sont **effectivement appliqués** | Test `T27` : `poids > poidsMax` → refus `POIDS` ; profil arme la garde |
| **W11** | `route()` compare les pointeurs → routes confondues | Comparaison **par chaîne uniquement** + drapeau de saturation | Test `T28` : 2 chaînes distinctes de même contenu → 1 entrée ; saturation signalée |
| **W12** | `autoCalibrer()` écrase `regler()` et la garde vaut 0,14·H | Les 4 formules documentées sont appliquées **exactement** | Test `T29` : `H=4700` → XOFF 3525, blocXoff 1645, garde 2585 / 1175 |
| **W13** | `regler()` tronque `concurrence` (300 → 44) et ne borne pas `partHeap` | Valeurs hors bornes **rejetées** (retour `false`) | Test `T30` : `regler(..., concurrence=300, ...)` → `false` |
| **W14** | `GE_ServerPrint` : destructeur ne flushe pas, erreur non propagée | Le destructeur vide le tampon ; `write()` retourne 0 si le serveur refuse | Test `T31` |
| **W15** | `GE_REFUS_DUREE` inatteignable | La cause est **supprimée** (aucun code mort) | `grep -c GE_REFUS_DUREE` = 0 |
| **W16** | Exemple 01 : `taille=1200` refusé en BODY sur 4 700 o de heap | L'exemple fonctionne avec un heap de 4 700 o | `pio run` + test `T32` simulant le heap |
| **W17** | 10 écarts documentation ↔ code | Chaque écart listé en F15 est corrigé | Revue manuelle + `grep` des affirmations |

## Suggestions retenues (4 sur 7)

| ID | Finding | Critère |
|---|---|---|
| **S1** | heap/bloc interrogés jusqu'à 5× par décision | **1 seul** échantillonnage par `admit()` |
| **S2** | `regler()` change `_bucketMs` sans invalider la fenêtre | `regler()` vide la fenêtre |
| **S3** | `sizeof < 300` vérifié à l'exécution | `static_assert(sizeof(Guard_ESP) < 300)` |
| **S4** | `X-RateLimit-Remaining` manquant | En-tête ajouté |

Reportées : copie de `GE_ServerPrint` (règle de 3) — sans consommateur ; chemin MCP
passant par `admit()` — évolution fonctionnelle, hors correctif.

---

## Critères de non-régression

| ID | Critère | Méthode |
|---|---|---|
| **N1** | `sizeof(Guard_ESP) < 300` | `static_assert` + mesure affichée |
| **N2** | Zéro `malloc`/`new`/`String` dans `Guard_ESP.h` | `grep -nE "malloc\|new \|String"` → seulement des commentaires |
| **N3** | 47 tests d'origine toujours verts | `./test_guard` → 0 échec |
| **N4** | `pio run` SUCCESS sur `esp12e` et `esp32dev`, 0 warning | `-Wall -Wextra` |
| **N5** | API `admit(poids,taille)` / `sortir()` / `resume(buf,len)` conservée | les tests d'origine compilent sans modification |
| **N6** | `Crash_ESP_Monitor` et `Server_MCP` inchangés | `git status` sur ces dépôts |
| **N7** | Les 3 profils gardent leurs valeurs numériques documentées | Test `T11` étendu |

---

## Definition of Done

Chaque finding est clos par : **diff + commande de vérification + résultat
attendu + statut `resolved` dans `changelog.md`**. Sinon `open` / `reporté`
(justifié).

## Critères de sortie du cycle

1. Les 5 critiques et 17 avertissements ont un critère **vérifié**.
2. `./test_guard` → **tous verts** (≥ 47 + nouveaux).
3. `pio run -e esp12e` et `-e esp32dev` → SUCCESS, 0 warning.
4. Les variantes `-D` interdites échouent **à la compilation**.
5. `README.md`, `LIVRE_BLANC.md`, `changelog.md` alignés sur le code.
6. `AGENTS.md` créé avec les invariants.
7. Étape 7 (compactage) et étape 8 (capitalisation) exécutées.
