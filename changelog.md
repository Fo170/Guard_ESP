# Changelog — Guard_ESP

Toutes les évolutions notables de la librairie.
Format : une section par version, avec les vérifications associées.

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
