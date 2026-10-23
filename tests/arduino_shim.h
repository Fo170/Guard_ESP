// Shim Arduino minimal pour tester Guard_ESP sur l'hôte (g++).
// Fournit : millis(), heap simulé, heap_caps_* (branche ESP32),
// et des faux serveurs pour exercer les adaptateurs.
#ifndef ARDUINO_H
#define ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

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

#endif // ARDUINO_H
