// Shim Arduino minimal pour tester Guard_ESP sur l'hôte (g++).
// Fournit : millis(), heap simulé, heap_caps_* (branche ESP32),
// et des faux serveurs pour exercer les adaptateurs.
#ifndef ARDUINO_H
#define ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <functional>

#define MALLOC_CAP_8BIT      (1u << 2)
#define MALLOC_CAP_INTERNAL  (1u << 11)

// Horloge simulée (ms)
extern uint32_t g_fakeMillis;
inline uint32_t millis() { return g_fakeMillis; }
inline void delay(uint32_t) {}

// Mémoire simulée
extern uint32_t g_fakeHeap;
extern uint32_t g_fakeBloc;

inline uint32_t heap_caps_get_free_size(uint32_t)           { return g_fakeHeap; }
inline uint32_t heap_caps_get_largest_free_block(uint32_t)  { return g_fakeBloc; }

struct FakeESP {
  uint32_t getFreeHeap()        const { return g_fakeHeap; }
  uint32_t getMaxFreeBlockSize() const { return g_fakeBloc; }
};
extern FakeESP ESP;

// ── Print minimal ───────────────────────────────────────────────
class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t*, size_t n) { return n; }
  virtual void flush() {}
  size_t print(const char* s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) write((uint8_t)s[i]);
    return n;
  }
};

// Print collecteur : permet de vérifier ce qui a été réellement écrit.
class FauxPrint : public Print {
public:
  size_t write(uint8_t c) override {
    if (total < sizeof(tampon) - 1) tampon[total] = (char)c;
    total++;
    return 1;
  }
  size_t write(const uint8_t* d, size_t n) override {
    for (size_t i = 0; i < n; i++) write(d[i]);
    return n;
  }
  void flush() override { flushs++; }

  char     tampon[512] = {};
  size_t   total       = 0;
  uint32_t flushs      = 0;
};

// ── String minimal (uniquement pour sendHeader) ─────────────────
class String {
public:
  String() {}
  String(const char* s) : _s(s ? s : "") {}
  const char* c_str() const { return _s; }
private:
  const char* _s = "";
};

// ── Faux serveur synchrone (ESP8266WebServer / WebServer) ───────
// Reproduit la surface réellement utilisée par GE_WebGuard :
//   sendHeader(const String&, const String&)
//   send(int, const char*, const char*)
class FauxServeur {
public:
  void sendHeader(const String& nom, const String& valeur) {
    (void)nom; (void)valeur;
    entetes++;
  }
  void send(int code, const char* ct, const char* corps) {
    (void)ct;
    dernierCode = code;
    snprintf(corpsRecu, sizeof(corpsRecu), "%s", corps ? corps : "");
  }
  void sendContent(const char* d, size_t n) { (void)d; envoye += n; }

  int      dernierCode = 0;
  int      entetes     = 0;
  size_t   envoye      = 0;
  char     corpsRecu[128] = {};
};

// ── Faux serveur asynchrone (AsyncWebServer) ────────────────────
class FauxAsyncResponse {
public:
  void addHeader(const String& nom, const String& valeur) {
    (void)nom; (void)valeur;
    entetes++;
  }
  int entetes = 0;
};

class FauxAsyncRequest {
public:
  FauxAsyncResponse* beginResponse(int code, const char* ct, const char* corps) {
    (void)ct;
    dernierCode = code;
    snprintf(corpsRecu, sizeof(corpsRecu), "%s", corps ? corps : "");
    _resp = FauxAsyncResponse();
    return &_resp;
  }
  void send(FauxAsyncResponse* r) { entetes = r ? r->entetes : 0; }
  void send(int code, const char* ct, const char* corps) {
    (void)ct;
    dernierCode = code;
    snprintf(corpsRecu, sizeof(corpsRecu), "%s", corps ? corps : "");
  }

  int  dernierCode = 0;
  int  entetes     = 0;
  char corpsRecu[128] = {};
private:
  FauxAsyncResponse _resp;
};

// ── Faux couple TCP : serveur / client / requête (GE_TcpGuard) ───
// Reproduit la surface réellement utilisée par Guard_ESP_tcp.h :
//   serveur  : onClient(callback, void* arg)
//   client   : setRxTimeout, write, close(bool), abort()
//   requête  : create(serveur*, client*) + onDisconnect(callback)
//
// ⚠️ Sur l'appareil, AsyncClient est ALLOUÉ SUR LE TAS : le garde fait
//    `delete c` sur le chemin de refus (comportement du framework). Le faux
//    client doit donc être créé avec `new` — un objet de pile provoquerait
//    « free(): invalid pointer ». Les tests lisent le résultat dans les
//    compteurs GLOBAUX ci-dessous, jamais dans l'objet (il est détruit).
// Compteurs globaux du faux framework (visibles par les tests).
extern int g_creations;      // AsyncWebServerRequest::create() appelés
extern int g_deconnexions;   // callbacks onDisconnect enregistrés
extern bool g_createEchoue;  // force l'échec d'allocation
extern std::function<void(void)> g_derniereDeconnexion;
extern char   g_dernierEcrit[256];
extern size_t g_dernierEcritLen;
extern bool   g_dernierClose;   // dernier close(bool) reçu
extern bool   g_dernierAbort;   // dernier abort() reçu

class FauxTcpClient {
public:
  FauxTcpClient()  { vivants++; }
  ~FauxTcpClient() { vivants--; }

  void setRxTimeout(int s) { rxTimeout = s; }
  void write(const char* d, size_t n) {
    if (n > sizeof(g_dernierEcrit) - 1) n = sizeof(g_dernierEcrit) - 1;
    memcpy(g_dernierEcrit, d, n);
    g_dernierEcrit[n] = '\0';
    g_dernierEcritLen = n;
    octetsEcrits += n;
  }
  void close(bool immediat) { ferme = true; fermetureImmediate = immediat; g_dernierClose = immediat; }
  void abort() { avorte = true; g_dernierAbort = true; }

  static int vivants;

  int    rxTimeout          = 0;
  bool   ferme              = false;
  bool   fermetureImmediate = false;
  bool   avorte             = false;
  size_t octetsEcrits       = 0;
};

// Clients possédés par le « framework » (créés par create(), libérés par
// fauxTcpLiberer()). Déclarés après FauxTcpClient : le type est complet ici.
extern FauxTcpClient* g_admis[8];
extern int g_nbAdmis;

class FauxTcpRequest {
public:
  static FauxTcpRequest* create(void* /*serveur*/, FauxTcpClient* client) {
    g_creations++;
    if (g_createEchoue) return nullptr;
    // Le framework prend possession du client : on le mémorise pour pouvoir
    // le libérer en fin de test (sinon LeakSanitizer signale une « fuite »
    // qui n'est que l'absence de serveur à arrêter).
    if (client && g_nbAdmis < 8) g_admis[g_nbAdmis++] = client;
    static FauxTcpRequest r;
    r._onDisconnect = nullptr;
    return &r;
  }
  void onDisconnect(std::function<void(void)> fn) {
    g_deconnexions++;
    _onDisconnect = fn;
    g_derniereDeconnexion = fn;   // pour simuler une fermeture côté client
  }

  std::function<void(void)> _onDisconnect;
};

inline void fauxTcpDeconnecter() { if (g_derniereDeconnexion) g_derniereDeconnexion(); }

// Simule l'arrêt du serveur : libère les clients admis (possédés par le
// framework sur l'appareil). À appeler en fin de test.
inline void fauxTcpLiberer() {
  for (int i = 0; i < g_nbAdmis; i++) delete g_admis[i];
  g_nbAdmis = 0;
}

class FauxTcpServeur {
public:
  void onClient(std::function<void(void*, FauxTcpClient*)> cb, void* arg) {
    _cb = cb;
    _arg = arg;
    installations++;
  }
  // Simule une connexion entrante (ce que fait AsyncServer::_accept).
  void connecter(FauxTcpClient* c) { if (_cb) _cb(_arg, c); }

  std::function<void(void*, FauxTcpClient*)> _cb;
  void* _arg          = nullptr;
  int   installations = 0;
};

#endif // ARDUINO_H
