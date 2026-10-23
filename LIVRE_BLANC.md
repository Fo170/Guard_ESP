# Livre blanc — Robustesse des IoT ESP8266 / ESP32

> Document de référence de `Guard_ESP`. Il recense les **49 problèmes réels**
> rencontrés sur le parc, les **12 bonnes pratiques** qui en découlent, les
> **valeurs de réglage typiques** par plateforme, et la **matrice de
> traçabilité** entre chaque mécanisme de la librairie et le problème qu'il
> prévient.
>
> Toutes les valeurs citées sont **mesurées**, pas estimées. Les dates
> renvoient aux incidents réels.

---

## 0. Principe fondateur

Deux librairies, deux responsabilités opposées :

| | `Crash_ESP_Monitor` | `Guard_ESP` |
|---|---|---|
| Verbe | **Observer** | **Protéger** |
| Nature | passif — ne modifie rien | actif — refuse une requête |
| Moment | **après** l'incident (dump au boot suivant) | **avant** l'incident |
| API | `CM_Statut()`, `CM_HeapOKBloc()` | `guard.admit()`, `guard.ticket()` |
| En cas de panne | rien n'est cassé | laisse passer **ou** bloque tout |

La dépendance est **optionnelle et à sens unique** :
`firmware → Guard_ESP → Crash_ESP_Monitor` (mesure mémoire uniquement).
`Crash_ESP_Monitor` n'est **jamais** modifié pour Guard_ESP.

---

## 1. Taxonomie — 8 familles, 49 problèmes

### Famille 1 — Épuisement mémoire (12)

| # | Problème | Cause racine | Conséquence | Correctif |
|---|---|---|---|---|
| M1 | `free()` sur pointeur non-`malloc` | erreur d'API | corruption, crash | corrigé |
| M2 | `x = ""` ne libère **rien** sur ESP8266 | `String` garde sa capacité | OTA échoue malgré « nettoyage » | `~String()` + reconstruction |
| M3 | Buffer non plafonné (`rawTrame` 10 000) | `substring()` recopie au lieu d'élaguer | heap à **3 412 o** | `remove()` en place |
| M4 | `JsonDocument` 2 048 o **sur la pile** | `StaticJsonDocument` local | exceptions + WDT aléatoires | document **statique en `.bss`** |
| M5 | Réponse construite en **une `String`** | pattern naïf | réponse tronquée (0/949/1072 o) | écriture **en flux** |
| M6 | `AsyncResponseStream` | accumule tout en RAM | pages > 9 Ko tronquées | `AsyncProgmemResponse` |
| M7 | `beginResponse(code, ct, const char*)` sans callback | copie la page en `String` | même troncature | idem M6 |
| M8 | Réponse en **2 documents** | `response["result"] = result` (deep-copy) | pic **3× la taille** → OOM dès 600 o | mono-document + stream O(1) |
| M9 | `SlotPool` jamais libéré | ressource oubliée | fuite lente | corrigé |
| M10 | `reserve()` sous-estimé | realloc en cascade | fragmentation | corrigé |
| M11 | Copie inutile de `struct` par requête | passage par valeur | gaspillage par appel | corrigé |
| M12 | Budget RAM statique trop gros | objets globaux | heap de démarrage 3 384 vs 4 456 o | `nm --size-sort` |

### Famille 2 — Fragmentation (3)

| # | Problème | Mesure |
|---|---|---|
| F1 | Fragmentation qui monte | 3 % au boot → 11 % après 5 requêtes |
| F2 | Heap suffisant mais **aucun bloc contigu** | 30 Ko libres → `malloc` échoue → `abort()` → panic (**31/08/2026**) |
| F3 | **La fragmentation n'est pas toujours le facteur** | 1 % de frag échoue, 8 % réussit (Coulometre F-20) |

### Famille 3 — Watchdog / blocage (5)

| # | Problème | Mesure |
|---|---|---|
| W1 | Loop bloqué par un handler long | **10 s** de gel sur 1 requête (Surveillance Batterie) |
| W2 | WDT reset | opération > 3 s → reboot (raison n°7 sur les 2 ESP32) |
| W3 | `ESP8266WebServer` **mono-client** | 2ᵉ requête attend ou est jetée |
| W4 | Auto-redémarrage pendant OTA | OTA coupée à 75 913 o |
| W5 | Device figé après upload avorté | **aucun correctif logiciel** → cycle d'alimentation |

### Famille 4 — Défaillances silencieuses (7)

La plus dangereuse : **le pire bug est celui qui répond `200`**.

| # | Problème | Pourquoi invisible |
|---|---|---|
| S1 | **`abort()` OOM ressemble à un reset logiciel** | `Crashs: 0`, `crash_recent: false` |
| S2 | Poussée Domoticz arrêtée **9 h 34** | rien ne mesure « depuis quand aucun envoi n'a réussi » |
| S3 | `HTTP 200` avec **corps vide** | le code HTTP dit « succès » |
| S4 | OTA échoue sans journaliser la cause | corrigé après 2 passes |
| S5 | Drapeaux à sens unique | corrigé |
| S6 | Motif `OK` sur un rejet de garde | corrigé |
| S7 | `abort()` muet | corrigé |

### Famille 5 — Protocole / HTTP (8)

| # | Problème | Correctif |
|---|---|---|
| P1 | Notification JSON-RPC sans corps | HTTP 202 vide (Server_MCP v1.1.2) |
| P2 | Réponse chunkée **bloquée à vie** | corrigé |
| P3 | `g_webConn` non rendu | corrigé |
| P4 | Message trompeur « canaux 0/2 » | v1.3.1 : 2 messages distincts |
| P5 | Écho `id` inexact (string/négatif/>2³²) | v1.3.2 |
| P6 | Validation de type laxiste | v1.3.2 (v7 stricte) |
| P7 | Pas de plafond de body | 413 (v1.3.2) |
| P8 | **429 ne couvre que `tools/call`** | ⚠️ **rien** → comblé par `Guard_ESP_web.h` |

### Famille 6 — Persistance (5)

| # | Problème | Conséquence |
|---|---|---|
| D1 | Rotation destructive non vérifiée | perte de données |
| D2 | Variance négative → NaN → **JSON invalide** | données corrompues |
| D3 | SoC inconnu publié à **0,0 %** | valeur fausse présentée comme vraie |
| D4 | `flushOnBoot()` inutile + commentaire faux | dette |
| D5 | `resetLearning()` laisse le segment vivant | état incohérent |

### Famille 7 — Concurrence / temps (4)

| # | Problème | Conséquence |
|---|---|---|
| C1 | Concurrence + JSON non échappé | corruption |
| C2 | `epoch` en `float` | Δt à 128 s |
| C3 | Lecture du port série hors du décodeur | **vole des octets** au décodeur |
| C4 | Rafale d'appels MCP lourds | panic n°4 (**31/08/2026**) |

### Famille 8 — Matériel / configuration (5)

| # | Problème | Conséquence |
|---|---|---|
| H1 | FRAM sans ACK I2C (err 5) | composant/câblage HS → repli EEPROM |
| H2 | SDA/SCL **inversés** entre doc et define | bus muet |
| H3 | DS18S20 N/A | mesure absente |
| H4 | Détection TIC laxiste | `Standard` avec 1200 bauds et 0 ligne OK |
| H5 | Plateforme non épinglée | OTA cassée après mise à jour |

---

## 2. Les 12 bonnes pratiques

### 2.1 Les 4 pièges documentés dans le code du parc

| Piège | Conséquence observée | Solution |
|---|---|---|
| `AsyncResponseStream` | pages > 9 Ko tronquées, JS cassé | `beginResponse(200, ct, (uint8_t*)page, strlen_P(page))` |
| `beginResponse(code, ct, const char*)` sans callback | `AsyncBasicResponse` **copie la page en `String`** | idem |
| Réponse en 2 documents | pic **3× la taille** → OOM dès 600 o | **un seul** document, `contents.clear()` avant envoi |
| Heap « suffisant » mais fragmenté | `malloc` échoue → `abort()` → panic | tester **heap ET plus gros bloc contigu** |

### 2.2 Les 8 règles

| # | Règle | Pourquoi | Comment |
|---|---|---|---|
| 1 | **Ne jamais bloquer le loop** | 1 requête = loop gelé 10 s ; au-delà du WDT → reboot | réponse immédiate puis travail long ; `yield()` ; jamais `delay()` |
| 2 | **Streamer, jamais matérialiser** | page 15 Ko = 15 Ko de heap, impossible sur 4,7 Ko | `Print` par blocs 128–256 o |
| 3 | **Flash par défaut** | chaque littéral en RAM est perdu à chaque `String` | `F()` / `PROGMEM` + `memcpy_P` |
| 4 | **Pondérer les requêtes** | `get_config` ≈ 30× `get_temperature` en coût | poids 1–3, rejet **avant** traitement |
| 5 | **XON/XOFF adaptatif** | seuil fixe = trop laxiste ou XOFF permanent | callback heap **et** bloc contigu |
| 6 | **Une seule requête lourde à la fois** | `ESP8266WebServer` mono-client | sémaphore + 503 + `Retry-After` |
| 7 | **Garde de démarrage** | démarrer le MCP à 4,3 Ko garantit l'OOM | refus si `heap < seuil` |
| 8 | **Journaliser en RTC, pas en RAM** | un log en `String` ne survit pas au crash | événements RTC + jalons |

### 2.3 Les 6 méta-leçons

**1. Un seuil mal placé est pire que pas de seuil.**
Telegram : garde à 35 Ko → XOFF **permanent**. Recalibré à 30 Ko. Un seuil doit rester **sous** le régime normal.

**2. Fragmentation et heap libre ne prédisent pas la même chose.**
31/08 : 30 Ko libres, aucun bloc contigu → `abort()`. F-20 : 1 % de frag échoue, 8 % réussit. Tester **les deux**, et savoir lequel protège quoi : *bloc contigu* pour les grosses allocations ponctuelles (TLS), *heap libre* pour le débit.

**3. Le pire bug est celui qui répond 200.**
200 + corps vide, 200 + valeur 0,0 %, reset « normal », OTA muette, Domoticz arrêté 9 h 34. La moitié des incidents réels sont des **succès apparents**.

**4. Le décodeur de crash est aveugle à l'OOM.**
`abort()` ressemble à `ESP.restart()` → `Crashs: 0`. D'où le **compteur de démarrages courts** (`CM_BOOT_COURT_MS 30 000`).

**5. Les tests couvraient les algorithmes, pas la plomberie.**
5 bugs ont survécu à 360 vérifications. Correctif : **shim capable d'injecter des pannes** (échec `write`, échec `rename`).

**6. Chaque protection crée son propre angle mort.**
Le garde anti-brique de 60 s **coupait l'OTA**. Le plafond de connexions peut rester bloqué si le compteur n'est pas rendu. Un limiteur de débit peut être contourné par le web.

---

## 3. Valeurs de réglage typiques

### 3.1 Base mesurée (relevés réels)

| Appareil | Puce | Heap libre | Heap min | Frag | Plus gros bloc |
|---|---|---|---|---|---|
| Thermo Salon | ESP8266 | 4 576 o | 3 128 o | 19 % | **3 707 o** |
| Thermo Chambre | ESP8266 | 4 752 o | 3 440 o | 18 % | **3 897 o** |
| Thermo Atelier | ESP8266 | 4 848 o | 3 040 o | 29 % | **3 442 o** |
| Relais-WIFI | ESP8266 | 20 288 o | 13 080 o | 13 % | **17 651 o** |
| Coulometre | ESP8266 | 6 456 o | — | — | ~5 200 o |
| Ordonnanceur | ESP32 | 52 756 o | 4 904 o | 15 % | **44 843 o** |
| Telegram | ESP32 | 76 636 o | 39 188 o | 10 % | **68 972 o** |

> `plus gros bloc ≈ heap × (100 − frag) / 100`

**L'ESP32 dispose de 13 à 18× plus de mémoire contiguë utilisable.** Les profils
sont donc **réellement distincts**, pas un simple jeu de constantes.

⚠️ **Le `heap min` de l'Ordonnanceur (4 904 o) n'est PAS un argument pour brider
l'ESP32.** C'est un pic transitoire (poignée de main TLS sortante) qui montre
qu'**une opération unique peut consommer ~48 Ko d'un coup** — d'où la garde sur
le bloc contigu. Le régime permanent, lui, est à 53 Ko.

### 3.2 Profil ESP8266 — ressource rare, concurrence interdite

| Paramètre | Valeur | Justification |
|---|---|---|
| Concurrence | **1** | mono-cœur, serveur web mono-client |
| Canaux / fenêtre | **4 / 30 s** | 8 est le défaut ESP32 ; 4 laisse 3,5 s entre appels |
| Poids max d'un outil | 3 | `get_config`, `get_graph_1h` = poids 3 |
| XOFF — heap | **4 200 o** | sous le régime normal (4,7 Ko) → jamais permanent |
| XOFF — bloc contigu | **1 800 o** | ~48 % du bloc typique |
| Max sous pression | **1** | un seul appel léger |
| Refus total (garde) | **3 000 / 1 200** | au niveau du minimum observé |
| Body max | **2 048 o** | 8 192 est structurellement impossible |
| Durée max requête | **800 ms** | WDT puce ≈ 3,2 s |
| Part du heap / réponse | **25 %** | |
| Budget | 8 req/min | 4/30 s |

### 3.3 Profil ESP32 — ressource abondante, concurrence autorisée

| Paramètre | Valeur | Justification |
|---|---|---|
| **Concurrence** | **3** | dual-core : web cœur 0, métier cœur 1 |
| Canaux / fenêtre | **8 / 30 s** | valeur éprouvée sur les 2 ESP32 |
| XOFF — heap | **25 000 o** | validé Telegram (35 Ko = trop haut) |
| XOFF — **bloc contigu** | **12 000 o** | ⚠️ indispensable |
| Max sous pression | **3** | poids 3 doit encore passer (leçon du 01/09) |
| Refus total | **20 000 / 10 000** | |
| Body max | **8 192 o** | valeur actuelle, adaptée |
| Durée max requête | **2 500 ms** | WDT configuré à 3 s |
| Part du heap / réponse | **10 %** | |
| Mesure mémoire | `heap_caps_*` par capacité | mesure exacte |

### 3.4 Paramètres communs

| Paramètre | Valeur |
|---|---|
| Fenêtre glissante | **30 000 ms** |
| `Retry-After` | **30 s** (= fenêtre) |
| Backoff sur erreur | 1 → 2 → 4 → 8 s, plafond **30 s** |
| Cooldown après reboot | **60 s** |

### 3.5 Auto-calibration

```cpp
guard.autoCalibrer();   // H_ref = heap après init WiFi + services
// → heapXoff    = H_ref × 0,75      blocXoff  = H_ref × 0,35
// → gardeHeap   = H_ref × 0,55      gardeBloc = H_ref × 0,25
// → canaux      = H_ref > 20000 ? 8 : (H_ref > 10000 ? 6 : 4)
// → concurrence = H_ref > 20000 ? 3 : 1
```

Vérification sur les mesures réelles :

| Appareil | H_ref | XOFF calculé | Régime | Canaux |
|---|---|---|---|---|
| Salon | 4 700 | 3 525 | 4 576 ✅ sous le régime | 4 |
| Relais | 20 300 | 15 225 | 20 288 ✅ | 8 |
| Telegram | 76 800 | 57 600 | 76 636 ✅ | 8 |

### 3.6 Anti-patrons de réglage

| Réglage dangereux | Effet | Correctif |
|---|---|---|
| XOFF au-dessus du régime normal | XOFF **permanent**, service dégradé (incident Telegram 35 Ko) | seuil **sous** le heap typique |
| Seuil sur le heap libre seul | 53 Ko libres, aucun bloc contigu → `abort()` | tester **heap ET bloc contigu** |
| `MAX_BODY` > heap disponible | garde 413 décorative | `MAX_BODY` ≤ 40 % du heap |
| `maxSousPression` = 2 avec outils poids 3 | outils **structurellement** rejetés (« canaux 0/2 ») | message distinct selon la cause |
| Fenêtre < 10 s | rejets en cascade sur usage légitime | 30 s par défaut |
| Canaux identiques ESP8266/ESP32 | 8 sur 4,7 Ko = OOM garanti | profil par plateforme |

---

## 4. Matrice de traçabilité — mécanisme ↔ problème

Chaque mécanisme de `Guard_ESP` répond à un problème **nommé** de la taxonomie.
Si un mécanisme est retiré, l'incident correspondant peut revenir.

| Mécanisme (`Guard_ESP`) | Problèmes couverts | Incident de référence |
|---|---|---|
| Fenêtre glissante pondérée + `Retry-After` | C4, P4, P7 | rafale MCP, panic n°4 du 31/08 |
| **Filtre de TOUTES les routes** (`Guard_ESP_web.h`) | **P8**, W1 | 429 contourné par le web |
| XON/XOFF heap **et** bloc contigu | F1, F2, M12 | panic du 31/08, XOFF permanent du 01/09 |
| Plafond de body ≤ 40 % du heap | P7, M3, M5 | `MCP_MAX_BODY 8192` sur 4,7 Ko |
| Concurrence + 503 | W3, W1 | gel 10 s (Surveillance Batterie) |
| Garde de durée par requête | W1, W2 | WDT reset n°7 |
| `GE_WebPrint` / `GE_ChunkPrint` | M5, M6, M7 | réponses 0/949/1072 o |
| Garde de démarrage `H_ref` | M12, S1 | MCP démarré à 4,3 Ko |
| Compteur de démarrages courts | S1 | `abort()` invisible (`Crashs: 0`) |
| **Watchdog d'inactivité** | S2 | Domoticz muet 9 h 34 (F-19) |
| Compteurs par route | S3 | `HTTP 200` + corps vide |
| Backoff + cooldown reboot | C4, W4 | OTA coupée, tempête de retries |

---

## 5. Ce que Guard_ESP ne fait pas

Pour rester honnête sur le périmètre :

- **Ne protège pas ce qui précède le handler** — décodage HTTP, TLS, parsing
  JSON d'entrée. La protection commence à l'entrée du handler, pas dans la
  pile TCP.
- **Ne remplace pas** `Server_MCP`, `ESP8266WebServer` ni `WebServer` : elle
  s'y **branche**.
- **Ne répare pas un bug applicatif** (DS18S20 muet, publication Domoticz figée).
- **N'empêche pas un crash matériel** (alimentation, capteur, RF).
- **Ne règle pas le plafond OTA à 24 453 o** (F14) : il est dans le cœur
  `Updater`, hors de portée d'une lib de filtrage.

---

## 6. Limites de ce document

- Les 49 problèmes viennent de **4 projets sur 8**. `PZEM004T`, `Station_Meteo`,
  `Surveillance_Batterie` et `Relais-WIFI` n'ont **aucun historique écrit** :
  leur absence de limiteur est un **risque non documenté**, pas une absence de
  risque.
- Les seuils sont dérivés d'**un relevé par appareil**, pas d'une campagne
  24 h. Une calibration terrain reste nécessaire.
- F-20 (1 % de frag échoue, 8 % réussit) **n'a pas de modèle explicatif
  unifié** → la lib mesure les deux sans prétendre que l'un suffit.
- Le pic `heap min` de l'Ordonnanceur mérite d'être tracé sur 24 h pour savoir
  s'il est exceptionnel ou récurrent.
