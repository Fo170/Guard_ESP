// ════════════════════════════════════════════════════════════════
// Exemple 2 — Guard_ESP + Server_MCP
//
// Montre la délégation : Guard_ESP décide les valeurs, Server_MCP
// applique le contrôle de flux et émet les 429.
//
// Remplace les constantes recopiées dans chaque projet
// (MCP_FLOW_MAX_REQ, MCP_PRESSURE_HEAP, MCP_HeapOK…) par une source
// de vérité unique, calibrée par plateforme.
//
// ⚠️ flux.appliquer() arme le callback de pression sur CETTE instance
//    de garde : ne pas compter sur guardInstance().
// ════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <Server_MCP.h>
#include <Guard_ESP.h>
#include <Guard_ESP_mcp.h>

Guard_ESP guard;
Server_MCP mcp("Exemple-Guard", "1.1.0", 4);
GE_McpFlow<Server_MCP> flux(guard);

static std::vector<MCPContent> outilLeger(const JsonObject&) {
  char buf[320];
  guard.resume(buf, sizeof(buf));
  return { Server_MCP::makeTextContent(String(buf)) };
}

static std::vector<MCPContent> outilLourd(const JsonObject&) {
  // Outil coûteux (lecture réseau + grosse réponse) : garde explicite.
  // Server_MCP applique déjà le XON/XOFF via le callback de flux, mais un
  // refus explicite donne un message clair à l'appelant.
  if (guard.enPression()) {
    return { Server_MCP::makeTextContent("Memoire insuffisante (XOFF) - reessayez") };
  }
  return { Server_MCP::makeTextContent("Reponse volumineuse OK") };
}

void setup() {
  Serial.begin(115200);
  delay(200);

  guard.profil(GE_PROFIL_AUTO);
  guard.autoCalibrer();
  guard.setGarde(3000, 1200);   // APRÈS autoCalibrer()
  guard.demarrer();

  // Poids : léger = 1, moyen = 2, lourd = 3
  mcp.registerTool("etat", "Etat du garde-fou", outilLeger, GE_McpFlow<Server_MCP>::poidsLeger());
  mcp.registerTool("gros", "Outil couteux", outilLourd, GE_McpFlow<Server_MCP>::poidsLourd());

  // Délègue canaux / fenêtre / max sous pression à Server_MCP
  flux.appliquer(mcp);
  mcp.begin(8081);

  char info[320];
  guard.resume(info, sizeof(info));
  Serial.println(info);
}

void loop() {
  mcp.handleClient();
  guard.tick();
}
