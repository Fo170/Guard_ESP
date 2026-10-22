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
// ════════════════════════════════════════════════════════════════

#ifndef GUARD_ESP_PRINT_H
#define GUARD_ESP_PRINT_H

#include <Arduino.h>

#ifndef GE_PRINT_BUF
#define GE_PRINT_BUF 192
#endif

// Écrit par blocs vers un serveur exposant sendContent().
// Compatible ESP8266WebServer, WebServer (ESP32) et AsyncWebServer.
template <typename ServerType>
class GE_ServerPrint : public Print {
public:
  explicit GE_ServerPrint(ServerType& s) : _srv(s) {}

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
    if (_len) {
      _srv.sendContent(_buf, _len);
      _len = 0;
    }
  }

  void flush() override { vider(); }

  size_t enAttente() const { return _len; }

private:
  ServerType& _srv;
  char   _buf[GE_PRINT_BUF];
  size_t _len = 0;
};

// Écrit par blocs vers un Print quelconque (Serial, SerialWeb, fichier).
class GE_ChunkPrint : public Print {
public:
  explicit GE_ChunkPrint(Print& cible) : _out(cible) {}

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
    if (_len) {
      _out.write((const uint8_t*)_buf, _len);
      _len = 0;
    }
  }

  void flush() override { vider(); }

private:
  Print& _out;
  char   _buf[GE_PRINT_BUF];
  size_t _len = 0;
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
