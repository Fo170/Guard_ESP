// ════════════════════════════════════════════════════════════════
// Exemple 1 — Guard_ESP + web + MCP (usage complet)
//
// Montre les 3 points d'intégration :
//   1. profil + auto-calibration au démarrage
//   2. filtre de TOUTES les routes web (y compris `/`, angle mort
//      du contrôle de flux MCP)
//   3. délégation des valeurs de flux à Server_MCP (pas de duplication)
// ════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <Guard_ESP.h>
#include <Guard_ESP_web.h>
#include <Guard_ESP_print.h>

#if defined(ESP8266)
  #include <ESP8266WebServer.h>
  ESP8266WebServer server(80);
#else
  #include <WebServer.h>
  WebServer server(80);
#endif

Guard_ESP guard;
GE_WebGuard<decltype(server)> wguard(guard);

static void pageAccueil() {
  // Filtre AVANT tout traitement : poids 2 (page moyenne), 1 200 o estimés.
  if (!wguard.admit(server, 2, 1200)) return;

  // Réponse streamée : aucune String, buffer fixe de 192 o.
  GE_WebPrint out(server);
  out.print(F("<!DOCTYPE html><html><head><meta charset='utf-8'>"));
  out.print(F("<title>Guard_ESP</title></head><body><h1>Protege</h1><pre>"));

  char ligne[320];
  guard.resume(ligne, sizeof(ligne));
  out.print(ligne);

  out.print(F("</pre></body></html>"));
  out.flush();
  server.sendContent("");

  guard.route("/");
  guard.routeOk();
  wguard.fin();
}

static void apiEtat() {
  if (!wguard.admit(server, 1, 600)) { guard.route("/api/etat"); guard.routeKo(); return; }

  char buf[320];
  guard.resume(buf, sizeof(buf));
  server.send(200, "text/plain", buf);

  guard.route("/api/etat");
  guard.routeOk();
  wguard.fin();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  guard.profil(GE_PROFIL_AUTO);   // ESP8266 → 4/30 s, ESP32 → 8/30 s
  guard.autoCalibrer();           // dérive les seuils de H_ref mesuré
  guard.setGarde(3000, 1200);     // refus de servir si la marge manque
  guard.demarrer();

  char info[320];
  guard.resume(info, sizeof(info));
  Serial.println(info);

  server.on("/", HTTP_GET, pageAccueil);
  server.on("/api/etat", HTTP_GET, apiEtat);
  server.begin();
}

void loop() {
  server.handleClient();
  guard.tick();   // watchdog d'inactivité + libération des slots bloqués
}
