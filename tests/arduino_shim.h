// Shim Arduino minimal pour tester Guard_ESP sur l'hôte (g++).
// Fournit : millis(), heap simulé, heap_caps_* (branche ESP32).
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
  uint32_t getFreeHeap()       const { return g_fakeHeap; }
  uint32_t getMaxFreeBlockSize() const { return g_fakeBloc; }
};
extern FakeESP ESP;

// Print minimal
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

#endif // ARDUINO_H
