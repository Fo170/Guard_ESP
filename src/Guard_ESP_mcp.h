// ════════════════════════════════════════════════════════════════
// Guard_ESP_mcp — adaptateur Server_MCP
//
// Répartition des rôles (aucune duplication) :
//   Guard_ESP  DÉCIDE  les valeurs (canaux, fenêtre, max sous pression)
//   Server_MCP APPLIQUE le contrôle de flux et émet les 429
//
// Cela remplace les constantes recopiées dans chaque projet
// (MCP_FLOW_MAX_REQ, MCP_PRESSURE_HEAP, MCP_HeapOK…) par une seule
// source de vérité, calibrée par profil de plateforme.
//
// Aucune dépendance dure : le type MCP est un paramètre de template,
// donc ce header n'inclut pas Server_MCP.h.
// ════════════════════════════════════════════════════════════════

#ifndef GUARD_ESP_MCP_H
#define GUARD_ESP_MCP_H

#include <Arduino.h>
#include "Guard_ESP.h"

// Callback de pression mémoire attendu par Server_MCP::setFlowPression().
// Signature imposée : bool(*)() — d'où le passage par l'instance globale.
inline bool GE_PressionCb() { return guardInstance().enPression(); }

template <typename McpType>
class GE_McpFlow {
public:
  explicit GE_McpFlow(Guard_ESP& g) : _g(g) {}

  // À appeler dans INIT_MCP(), AVANT mcp.begin().
  void appliquer(McpType& mcp) {
    mcp.setFlowControl(_g.canaux(), _g.fenetreMs());
    mcp.setFlowPression(GE_PressionCb, _g.maxSousPression());
    _applique = true;
  }

  // Poids recommandés par type d'outil (1 léger … 3 lourd).
  // Évite qu'une rafale d'outils lourds sature la fenêtre 3× plus vite.
  static uint8_t poidsLeger()  { return 1; }
  static uint8_t poidsMoyen()  { return 2; }
  static uint8_t poidsLourd()  { return 3; }

  bool applique() const { return _applique; }

  // Ligne de diagnostic pour CM_Statut() / /etat.
  size_t resume(char* buf, size_t len) const { return _g.resume(buf, len); }

private:
  Guard_ESP& _g;
  bool _applique = false;
};

#endif // GUARD_ESP_MCP_H
