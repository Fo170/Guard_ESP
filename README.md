# Guard_ESP

Bibliothèque de **protection** pour **ESP8266** et **ESP32** : elle décide
d'**admettre, ralentir ou rejeter** une requête **avant** que l'appareil n'ait
consommé la moindre ressource fatale.

> **Auteur** : Olivier FOURNET · **Licence** : GPL-3.0-only · **Version** : 1.0.0
> **Compatibilité** : ESP8266 (NodeMCU, Wemos D1) et ESP32 (DevKit, Wemos D1 Mini ESP32)
> sous **Arduino IDE** et **PlatformIO**
> **Header-only** · **zéro allocation dynamique** · `sizeof(Guard_ESP)` = **232 octets**

---

## Observer vs protéger

| | [`Crash_ESP_Monitor`](https://github.com/Fo170/Crash_ESP_Monitor) | **`Guard_ESP`** |
|---|---|---|
| Verbe | **Observer** | **Protéger** |
| Nature | passif — mesure, journalise, diagnostique | actif — refuse une requête |
| Moment | **après** l'incident (dump au boot suivant) | **avant** l'incident |
| API | `CM_Statut()`, `CM_HeapOKBloc()` | `guard.admit()`, `guard.resume()` |
| En cas de panne | rien n'est cassé | laisse passer **ou** bloque tout |

Les deux sont **complémentaires et indépendantes**. `Guard_ESP` reste autonome ;
s'il détecte `Crash_ESP_Monitor` (`__has_include`), il en réutilise la mesure
mémoire. **`Crash_ESP_Monitor` n'est jamais modifié.**

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

Détail complet, taxonomie de **34 problèmes** et **12 bonnes pratiques** :
[`LIVRE_BLANC.md`](LIVRE_BLANC.md).

---

## Installation

### PlatformIO

```ini
lib_deps =
    https://github.com/Fo170/Guard_ESP.git@^1.0.0
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
  guard.tick();                   // watchdog d'inactivité
}
```

### 2. Web — filtre TOUTES les routes

```cpp
#include <Guard_ESP_web.h>

GE_WebGuard<decltype(server)> wguard(guard);

void maRoute() {
  if (!wguard.admit(server, 2, 1200)) return;   // refus déjà envoyé (429/503)
  // ... traitement ...
  wguard.fin();
}
```

Le refus envoie automatiquement `Retry-After`, `X-RateLimit-Limit`,
`X-RateLimit-Used`, `X-RateLimit-Window` et `X-Guard-Reason`.

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
| `admit(poids, taille)` | `true` = la requête peut être traitée |
| `sortir()` | rend le slot de concurrence, mesure la durée |
| `tick()` | à appeler dans `loop()` : libère les slots bloqués |
| `activite()` | signale une activité métier (anti-inactivité) |

### Diagnostic

| Méthode | Rôle |
|---|---|
| `refus()` / `refusTexte()` | cause du dernier refus |
| `retryAfterMs()` | délai à annoncer au client |
| `resume(buf, len)` | résumé texte **sans `String`** |
| `route(nom)` / `routeOk()` / `routeKo()` | compteurs par route |
| `admis()` / `rejetes()` / `rejets(cause)` | compteurs globaux |
| `dureePire()` / `depassements()` | tenue des handlers |
| `inactif()` / `inactiviteMs()` | watchdog |

### Causes de refus

`GE_REFUS_FLUX` · `GE_REFUS_POIDS` · `GE_REFUS_PRESSION` ·
`GE_REFUS_CONCURRENCE` · `GE_REFUS_BODY` · `GE_REFUS_BACKOFF` ·
`GE_REFUS_GARDE` · `GE_REFUS_DUREE`

Codes HTTP : **429** pour `FLUX` / `POIDS` / `BACKOFF` (le client peut
réessayer), **503** pour les autres (patienter).

---

## Les 7 risques couverts

| # | Risque | Mécanisme |
|---|---|---|
| 1 | OOM par rafale | fenêtre glissante **pondérée** (429) |
| 2 | OOM par réponse trop grosse | plafond de taille **avant** allocation |
| 3 | Freeze / watchdog | plafond de concurrence (503) + garde de durée |
| 4 | Heap bas | XON/XOFF sur le heap libre |
| 5 | Fragmentation | XON/XOFF sur le **plus gros bloc contigu** |
| 6 | Tempête de retries | backoff + cooldown après reboot |
| 7 | Démarrage en RAM faible | garde `H_ref` |

---

## Profils

| Paramètre | ESP8266 | ESP32 |
|---|---|---|
| Concurrence | 1 | 2–4 |
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

**47 vérifications**, chacune référençant le problème réel qu'elle protège.
Empreinte vérifiée : `sizeof(Guard_ESP) = 232 octets`.

Compilation croisée des exemples :

```bash
cd examples/01_web_basic && pio run -e esp12e && pio run -e esp32dev
cd examples/02_mcp_basic && pio run -e esp12e && pio run -e esp32dev
```

---

## Ce que la librairie ne fait pas

- **Ne protège pas ce qui précède le handler** — décodage HTTP, TLS, parsing
  JSON d'entrée. La protection commence à l'entrée du handler.
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
| `src/Guard_ESP_print.h` | `Print` streamé sans `String` |
| `examples/01_web_basic` | web + page streamée |
| `examples/02_mcp_basic` | intégration `Server_MCP` |
| `tests/` | 47 vérifications hôte + shim Arduino |
| `LIVRE_BLANC.md` | taxonomie, bonnes pratiques, valeurs typiques |
