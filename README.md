# Guard_ESP

Bibliothèque de **protection** pour **ESP8266** et **ESP32** : elle décide
d'**admettre, ralentir ou rejeter** une requête **avant** que l'appareil n'ait
consommé la moindre ressource fatale.

> **Auteur** : Olivier FOURNET · **Licence** : GPL-3.0-only · **Version** : 1.2.0
> **Compatibilité** : ESP8266 (NodeMCU, Wemos D1) et ESP32 (DevKit, Wemos D1 Mini ESP32)
> sous **Arduino IDE** et **PlatformIO**
> **Header-only** · **zéro allocation dynamique** · `sizeof(Guard_ESP)` = **256 octets**

---

## Observer vs protéger

| | [`Crash_ESP_Monitor`](https://github.com/Fo170/Crash_ESP_Monitor) | **`Guard_ESP`** |
|---|---|---|
| Verbe | **Observer** | **Protéger** |
| Nature | passif — mesure, journalise, diagnostique | actif — refuse une requête |
| Moment | **après** l'incident (dump au boot suivant) | **avant** l'incident |
| API | `CM_Statut()`, `CM_HeapOKBloc()` | `guard.admit()`, `guard.resume()` |
| En cas de panne | rien n'est cassé | laisse passer **ou** bloque tout |

Les deux sont **complémentaires et indépendantes**. `Guard_ESP` est **totalement
autonome** : il mesure la mémoire lui-même (`ESP.getFreeHeap()` /
`heap_caps_get_free_size()`), sans dépendre de `Crash_ESP_Monitor` ni d'aucune
autre bibliothèque. **`Crash_ESP_Monitor` n'est jamais modifié**, et aucun
`__has_include` n'est utilisé.

---

## Pourquoi cette librairie

Mesures réelles sur le parc — le contrôle de flux existant ne couvrait que
`tools/call` :

| Constat mesuré | Conséquence |
|---|---|
| `initialize`, `tools/list`, `ping` et **l'interface web `/`** illimités | un scraping web écroule un ESP8266 **sans jamais déclencher de 429** |
| 4 serveurs sur 6 sans backpressure mémoire | XOFF inexistant alors que le heap est à 4,7 Ko |
| 4 serveurs sur 6 sans pondération d'outils | 8 outils lourds autorisés en 30 s sur 4,7 Ko |
| 4 serveurs sur 6 sans garde de démarrage | MCP démarré à 4,3 Ko de heap |
| 5 IoT web sans aucune protection | **1 requête bloque le loop 10 s** |
| 4 projets tournent avec les **défauts ESP32** (8 req/30 s) | inadapté à 15× moins de mémoire |

Détail complet, taxonomie de **49 problèmes** (8 familles) et **12 bonnes pratiques** :
[`LIVRE_BLANC.md`](LIVRE_BLANC.md).

---

## Installation

### PlatformIO

```ini
lib_deps =
    https://github.com/Fo170/Guard_ESP.git@^1.2.0
```

### Arduino IDE

Télécharger le dépôt, puis **Croquis → Inclure une bibliothèque → Ajouter la
bibliothèque .ZIP**. Le dossier doit s'appeler `Guard_ESP` et contenir `src/`.

---

## Utilisation rapide

### 1. Noyau seul

```cpp
#include <Guard_ESP.h>

Guard_ESP guard;

void setup() {
  guard.profil(GE_PROFIL_AUTO);   // ESP8266 → 4/30 s · ESP32 → 8/30 s
  guard.autoCalibrer();           // dérive les seuils de H_ref mesuré
  guard.setGarde(3000, 1200);     // refus de servir si la marge manque
  guard.demarrer();
}

void loop() {
  guard.tick();                   // libère les slots d'un handler bloqué
}
```

### 2. Web — filtre TOUTES les routes

```cpp
#include <Guard_ESP_web.h>

GE_WebGuard<decltype(server)> wguard(guard);

void maRoute() {
  GE_Ticket t = wguard.admit(server, 2, 192);   // 192 = plus gros bloc alloué
  if (!t) return;                               // refus déjà envoyé (429/503)
  // ... traitement : le slot est libéré à la sortie de portée ...
}
```

⚠️ **Ne jamais écrire `if (!wguard.admit(...)) return;`** : le ticket serait
détruit à la fin de l'instruction et le slot rendu **avant** le traitement.
Toujours stocker le ticket dans une variable. `admit()` est marqué
`warn_unused_result` pour que le compilateur signale l'erreur.

Le refus envoie `Retry-After`, `X-RateLimit-Limit`, `X-RateLimit-Used`,
`X-RateLimit-Remaining`, `X-RateLimit-Window` et `X-Guard-Reason` — **sauf**
si l'appareil est déjà sous pression mémoire, où aucun en-tête n'est
construit (pas de `String` au pire moment).

### 2 bis. TCP — plafond AVANT toute allocation

```cpp
#include <Guard_ESP.h>
#include <Guard_ESP_tcp.h>

GE_TcpGuard<AsyncWebServer, AsyncClient, AsyncWebServerRequest>
    tcp(guard, server, 3);   // 3 connexions simultanées

// APRÈS la construction du serveur (installer() REMPLACE son onClient)
tcp.installer();
```

`GE_WebGuard` (niveau handler) décide **après** la création de
l'`AsyncWebServerRequest` : sur un ESP8266 sous pression, c'est déjà trop tard —
l'allocation a échoué et le panic est déclenché (`_parseReqHeader()`,
mesuré le 02/10/2026 : reboot loop toutes les 5-20 s). `GE_TcpGuard` décide
dans `onClient()`, donc **avant** toute allocation.

Deux refus, dans cet ordre :

1. **garde mémoire** — `guard.gardePassee()` faux (heap ou plus gros bloc
   contigu sous le seuil) → **arrêt du service** : mieux vaut ne pas servir
   que planter ;
2. **plafond de connexions** — `setMaxConnexions(n)`.

Le refus est **construit sur la pile** (`snprintf`, 192 o) : aucune `String`,
aucun `new`, donc il réussit même à 200 o de heap. Il renvoie `503`,
`Retry-After`, `X-RateLimit-Limit` et `X-Guard-Reason`.

```cpp
tcp.setGardeMemoire(false);   // diagnostic : ne pas couper sous pression
tcp.setMaxConnexions(5);
char buf[128]; tcp.resume(buf, sizeof(buf));   // diagnostic, sans String
```

⚠️ `AsyncWebServer` n'expose **pas** `onClient` : le handler vit sur son membre
protégé `_server`. Pour ce cas, ré-exposer le handler dans une classe dérivée et
passer l'**enregistreur** :

```cpp
class MonServeur : public AsyncWebServer {
public:
  MonServeur(uint16_t p) : AsyncWebServer(p) {}
  void onClientConn(AcConnectHandler cb) { _server.onClient(cb, this); }
};

GE_TcpGuard<MonServeur, AsyncClient, AsyncWebServerRequest> tcp(guard, server, 3);
tcp.installer([](AcConnectHandler cb) { server.onClientConn(cb); });
```

⚠️ Le garde doit vivre **aussi longtemps que le serveur** (les callbacks
`onDisconnect` capturent son adresse) : instance globale ou membre, jamais
un objet de pile dans `setup()`.

### 3. MCP — délégation à `Server_MCP`

```cpp
#include <Server_MCP.h>
#include <Guard_ESP_mcp.h>

GE_McpFlow<Server_MCP> flux(guard);

mcp.registerTool("etat", "État", cbEtat, GE_McpFlow<Server_MCP>::poidsLeger());
mcp.registerTool("log",  "Log",  cbLog,  GE_McpFlow<Server_MCP>::poidsLourd());

flux.appliquer(mcp);   // AVANT mcp.begin()
```

`Guard_ESP` **décide** les valeurs ; `Server_MCP` **applique** et émet les 429.
Aucune duplication de constantes.

### 4. Page streamée sans `String`

```cpp
#include <Guard_ESP_print.h>

GE_WebPrint out(server);          // buffer fixe de 192 o

char buf[320];
guard.resume(buf, sizeof(buf));   // resume() écrit dans un buffer, sans String

out.print(F("<html><body><pre>"));
out.print(buf);
out.print(F("</pre></body></html>"));
out.flush();
server.sendContent("");
```

`resume()` n'utilise **pas** `String` : il écrit dans un buffer fourni par
l'appelant, et retourne le nombre de caractères écrits.

---

## API

### Configuration

| Méthode | Rôle |
|---|---|
| `profil(GE_ProfilId)` | applique un profil complet (`AUTO`, `ESP8266`, `ESP32`) |
| `regler(...)` | réglage fin de tous les paramètres |
| `autoCalibrer()` | dérive les seuils du heap mesuré au démarrage |
| `setGarde(heapMin, blocMin)` | refuse de servir sous ce seuil |
| `setBackoff(baseMs, maxMs)` | backoff exponentiel sur erreur |
| `setInactivite(seuilMs)` | watchdog d'inactivité |
| `demarrer()` | arme le cooldown post-reboot |

### Décision

| Méthode | Rôle |
|---|---|
| `admit(poids, taille)` | `true` = slot acquis, à rendre par `sortir()` (usage mono-requête) |
| `ticket(poids, taille)` | **recommandé** : jeton RAII, libération automatique et exacte |
| `sortir()` / `sortir(idx)` | rend le slot du dernier `admit()` / un slot précis |
| `tick()` | à appeler dans `loop()` : libère les slots restés ouverts trop longtemps |
| `activite()` | signale une activité métier (anti-inactivité) |

### Diagnostic et lecture

| Méthode | Rôle |
|---|---|
| `refus()` / `refusTexte(r)` | cause du dernier refus |
| `retryAfterMs()` | délai à annoncer au client |
| `resume(buf, len)` | résumé texte **sans `String`** (retour ≤ `len-1`), **mutant** |
| `route(nom)` / `routeOk()` / `routeKo()` | compteurs par route |
| `routes()` / `routesSaturees()` | table des routes, drapeau de saturation |
| `admis()` / `rejetes()` / `rejets(cause)` | compteurs globaux |
| `dureePire()` / `depassements()` | tenue des handlers |
| `inactif()` / `inactiviteMs()` / `activite()` | watchdog d'inactivité |
| `canaux()` / `poidsMax()` / `fenetreMs()` | réglage actif |
| `maxSousPression()` | **poids** max sous pression (1–3), pas un seuil mémoire |
| `heapXoff()` / `blocXoff()` | seuils mémoire du XOFF (octets) |
| `heapRefus()` / `blocRefus()` | seuils de la garde de démarrage (octets) |
| `chargeCourante()` / `creditRestant()` | charge de la fenêtre, crédit restant (**mutants**) |
| `xoff()` / `actifs()` / `actif()` / `actif(bool)` | état courant |
| `concurrence()` / `bodyMax()` / `dureeMaxMs()` | réglage actif |
| `profilId()` / `profilTexte()` | profil appliqué |
| `enPression()` / `gardePassee()` | tests mémoire |
| `heapLibre()` / `blocLibre()` (static) | mesures brutes |

### Causes de refus

`GE_REFUS_FLUX` · `GE_REFUS_POIDS` · `GE_REFUS_PRESSION` ·
`GE_REFUS_CONCURRENCE` · `GE_REFUS_BODY` · `GE_REFUS_BACKOFF` ·
`GE_REFUS_GARDE` · `GE_REFUS_INACTIF`

Codes HTTP : **429** pour `FLUX` / `BACKOFF` (le client peut réessayer plus
tard), **503** pour les autres. `POIDS` est un refus **structurel** (l'outil est
trop lourd pour l'état de flux) : réessayer ne changera rien, d'où 503.

---

## Les 8 risques couverts

| # | Risque | Mécanisme |
|---|---|---|
| 1 | OOM par rafale | fenêtre glissante **pondérée** (429) |
| 2 | OOM par réponse trop grosse | plafond de taille **avant** allocation |
| 3 | Freeze / watchdog | plafond de concurrence (503) + garde de durée |
| 4 | Heap bas | XON/XOFF sur le heap libre |
| 5 | Fragmentation | XON/XOFF sur le **plus gros bloc contigu** |
| 6 | Tempête de retries | backoff + cooldown après reboot |
| 7 | Démarrage en RAM faible | garde `H_ref` |
| 8 | OOM **avant** le handler (création de la requête) | `GE_TcpGuard` : plafond TCP, refus sans allocation |

---

## Profils

| Paramètre | ESP8266 | ESP32 |
|---|---|---|
| Concurrence | 1 | 3 |
| Canaux / fenêtre | 4 / 30 s | 8 / 30 s |
| XOFF heap / bloc | 4 200 / 1 800 | 25 000 / 12 000 |
| Refus total | 3 000 / 1 200 | 20 000 / 10 000 |
| Body max | 2 048 | 8 192 |
| Durée max requête | 800 ms | 2 500 ms |
| Part du heap / réponse | 25 % | 10 % |

Justification et mesures : [`LIVRE_BLANC.md` §3](LIVRE_BLANC.md#3-valeurs-de-réglage-typiques).

---

## Tests

Les tests tournent sur l'hôte (aucun matériel requis) :

```bash
cd Guard_ESP
g++ -std=c++17 -Wall -Wextra -I tests -I src tests/test_guard.cpp -o test_guard
./test_guard
```

**192 vérifications**, chacune référençant le problème réel qu'elle protège.
Empreinte vérifiée : `sizeof(Guard_ESP) = 256 octets` (garanti par `static_assert`).

Compilation croisée des exemples :

```bash
cd examples/01_web_basic && pio run -e esp12e && pio run -e esp32dev
cd examples/02_mcp_basic && pio run -e esp12e && pio run -e esp32dev
```

---

## Ce que la librairie ne fait pas

- **Ne protège pas ce qui précède le handler** — décodage HTTP, TLS, parsing
  JSON d'entrée. La protection commence à l'entrée du handler.
- **Les accesseurs mutants** (`resume`, `chargeCourante`, `creditRestant`)
  font vieillir la fenêtre : les réserver à la tâche propriétaire du garde.
- **`route(nom)`** exige une chaîne de durée de vie ≥ celle du programme.
- **Ne remplace pas** `Server_MCP`, `ESP8266WebServer` ni `WebServer`.
- **Ne répare pas un bug applicatif** (capteur muet, publication figée).
- **N'empêche pas un crash matériel** (alimentation, capteur, RF).
- **Ne règle pas le plafond OTA** à 24 453 o : il est dans le cœur `Updater`.

---

## Fichiers

| Fichier | Contenu |
|---|---|
| `src/Guard_ESP.h` | noyau : fenêtre, pression, concurrence, garde, stats |
| `src/Guard_ESP_web.h` | filtre `ESP8266WebServer` / `WebServer` / `AsyncWebServer` |
| `src/Guard_ESP_mcp.h` | adaptateur `Server_MCP` |
| `src/Guard_ESP_tcp.h` | plafond de connexions TCP + garde mémoire **avant** toute allocation |
| `src/Guard_ESP_print.h` | `Print` streamé sans `String` |
| `examples/01_web_basic` | web + page streamée |
| `examples/02_mcp_basic` | intégration `Server_MCP` |
| `tests/` | 192 vérifications hôte + shim Arduino (dont faux serveurs web et faux couple TCP) |
| `LIVRE_BLANC.md` | taxonomie, bonnes pratiques, valeurs typiques |
| `analyse.md` / `criteres.md` | cycle de chasse aux bugs n°1 |
| `AGENTS.md` | invariants à ne pas casser, pièges connus |
