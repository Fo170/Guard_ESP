// ════════════════════════════════════════════════════════════════
// Guard_ESP_print — Print streamé SANS String
//
// Remplace le pattern « page entière dans une String » qui a causé des
// réponses tronquées (0 / 949 / 1072 o) sur LinkysTIC et 200 + corps vide
// sur Coulometre. Buffer FIXE, flush par blocs : mémoire O(1) quel que soit
// la taille de la page.
//
// ⚠️ Le WrapperPrint existant alloue encore (`String s; s.reserve(size)`)
//    pour les écritures >= 128 o. Ici : aucune allocation, jamais.
//
// ⚠️ Le destructeur VIDE le tampon : sans cela, les derniers octets
//    étaient perdus silencieusement (finding W14) — exactement le mode
//    d'échec « corps tronqué » que cette classe existe pour supprimer.
// ════════════════════════════════════════════════════════════════

#ifndef GUARD_ESP_PRINT_H
#define GUARD_ESP_PRINT_H

#include <Arduino.h>

#ifndef GE_PRINT_BUF
#define GE_PRINT_BUF 192
#endif

static_assert(GE_PRINT_BUF >= 16, "GE_PRINT_BUF doit etre >= 16");

// Écrit par blocs vers un serveur exposant sendContent(const char*, size_t).
// Compatible ESP8266WebServer et WebServer (ESP32).
// ⚠️ PAS AsyncWebServer : cette classe n'expose pas sendContent().
template <typename ServerType>
class GE_ServerPrint : public Print {
public:
  explicit GE_ServerPrint(ServerType& s) : _srv(&s) {}
  ~GE_ServerPrint() { vider(); }

  GE_ServerPrint(const GE_ServerPrint&) = delete;
  GE_ServerPrint& operator=(const GE_ServerPrint&) = delete;

  size_t write(uint8_t c) override {
    _buf[_len++] = (char)c;
    if (_len >= sizeof(_buf)) vider();
    return 1;
  }

  size_t write(const uint8_t* data, size_t n) override {
    size_t i = 0;
    while (i < n) {
      size_t place = sizeof(_buf) - _len;
      size_t bloc  = (n - i < place) ? (n - i) : place;
      memcpy(_buf + _len, data + i, bloc);
      _len += bloc;
      i += bloc;
      if (_len >= sizeof(_buf)) vider();
    }
    return n;
  }

  void vider() {
    if (_len && _srv) {
      _srv->sendContent(_buf, _len);
      _len = 0;
    }
  }

  void flush() override { vider(); }

  size_t enAttente() const { return _len; }

private:
  ServerType* _srv;
  char        _buf[GE_PRINT_BUF];
  size_t      _len = 0;
};

// Écrit par blocs vers un Print quelconque (Serial, SerialWeb, fichier).
// Contrairement à GE_ServerPrint, write() propage l'échec de la cible :
// si la cible n'accepte plus d'octets, write() retourne 0 au lieu de
// prétendre avoir tout écrit (finding W14).
class GE_ChunkPrint : public Print {
public:
  explicit GE_ChunkPrint(Print& cible) : _out(&cible) {}
  ~GE_ChunkPrint() { vider(); }

  GE_ChunkPrint(const GE_ChunkPrint&) = delete;
  GE_ChunkPrint& operator=(const GE_ChunkPrint&) = delete;

  size_t write(uint8_t c) override {
    _buf[_len++] = (char)c;
    if (_len >= sizeof(_buf)) vider();
    return _erreur ? 0 : 1;
  }

  size_t write(const uint8_t* data, size_t n) override {
    size_t i = 0;
    while (i < n) {
      size_t place = sizeof(_buf) - _len;
      size_t bloc  = (n - i < place) ? (n - i) : place;
      memcpy(_buf + _len, data + i, bloc);
      _len += bloc;
      i += bloc;
      if (_len >= sizeof(_buf)) vider();
    }
    return _erreur ? 0 : n;
  }

  void vider() {
    if (_len && _out) {
      size_t ecrit = _out->write((const uint8_t*)_buf, _len);
      if (ecrit != _len) _erreur = true;
      _len = 0;
    }
  }

  void flush() override {
    vider();
    if (_out) _out->flush();
  }

  bool erreur() const { return _erreur; }
  size_t enAttente() const { return _len; }

private:
  Print* _out;
  char   _buf[GE_PRINT_BUF];
  size_t _len    = 0;
  bool   _erreur = false;
};

// ────────────────────────────────────────────────────────────────
// Alias pratiques (évite CTAD, non disponible en C++11 sur ESP32)
// ────────────────────────────────────────────────────────────────
#if defined(ESP8266)
  #include <ESP8266WebServer.h>
  using GE_WebPrint = GE_ServerPrint<ESP8266WebServer>;
#elif defined(ESP32)
  #include <WebServer.h>
  using GE_WebPrint = GE_ServerPrint<WebServer>;
#endif

#endif // GUARD_ESP_PRINT_H
