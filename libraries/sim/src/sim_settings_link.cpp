// The settings' "Link play" page: the DS's local wireless between two devices running the
// game (sim_net.cpp relays it over the network). One device hosts, the other joins: from
// the hosts found on the local network, or by address (another network through a VPN).
#include "simulator/config/sim_config.h"
#include "simulator/imgui/imgui.hpp"
#include "simulator/sim.h"
#include "simulator/sim_files.h"
#include "simulator/sim_net.h"
#include "simulator/sim_settings.hpp"
#include "simulator/sim_ui.hpp"

#include <cstdio>
#include <cstring>
#include <string>

// Game hook (weak default: none): where in the game the link is used ("Upstairs in any
// Pokemon Center: ...")
extern "C" const char *__attribute__((weak)) SIM_Prj_LinkPlayHint(void) {
  return nullptr;
}

namespace SIM::Settings {

namespace {

using namespace SIM::UI;

std::string sStatus;
bool sStatusError = false;
bool sAddressOpen = false;
int sOctets[4] = { 192, 168, 1, 2 };
bool sOctetsLoaded = false;

void LoadAddress() {
  if (sOctetsLoaded) {
    return;
  }
  sOctetsLoaded = true;
  // The last address joined, else this device's network with a guess at the host
  unsigned a, b, c, d;
  const SIM_config_type *config = SIM_GetConfigPtr();
  char local[32];
  if (config != nullptr && sscanf(config->netSettings.serverIp, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
      !(a == 127 && b == 0 && c == 0 && d == 1)) {
  } else if (SIM_Net_LocalAddress(local, sizeof(local)) && sscanf(local, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
    d = 2;
  } else {
    a = 192, b = 168, c = 1, d = 2;
  }
  sOctets[0] = (int)a, sOctets[1] = (int)b, sOctets[2] = (int)c, sOctets[3] = (int)d;
}

void Join(const char *address) {
  // Remembered for next time (and the command line's --net-client)
  SIM_config_type *config = SIM_GetConfigPtr();
  if (config != nullptr) {
    snprintf(config->netSettings.serverIp, sizeof(config->netSettings.serverIp), "%s", address);
    SIM_Config_SaveConfigFile(config);
  }
  if (SIM_Net_StartClientAsync(address)) {
    sStatus = std::string("Joining ") + address + "...";
    sStatusError = false;
  } else {
    sStatus = "Couldn't start joining.";
    sStatusError = true;
  }
}

void DrawLinked(SIM_Net_State_t state) {
  const SIM_Net_Player_t *players = SIM_Net_GetPlayerList();
  const int me = SIM_Net_GetCurrentPlayerID();
  int others = 0;
  for (int i = 0; i < SIM_NET_MAX_PLAYERS; i++) {
    if (i != me && (players[i].status == SIM_NET_PLAYERSTATUS_CLIENT || players[i].status == SIM_NET_PLAYERSTATUS_HOST)) {
      others++;
    }
  }

  if (state == SIM_NET_STATE_HOST) {
    char address[32] = "";
    SIM_Net_LocalAddress(address, sizeof(address));
    std::string text = others == 0 ? "Hosting. Waiting for the other device to join"
                                   : (others == 1 ? "Hosting: 1 device joined" : "Hosting: " + std::to_string(others) + " devices joined");
    Status(text.c_str());
    std::string where = address[0] != '\0' ? std::string("This device's address: ") + address +
                                                 ". On the same Wi-Fi, the other device finds it by itself."
                                           : std::string("On the same Wi-Fi, the other device finds this one by itself.");
    Note(where.c_str());
  } else if (SIM_Net_HostLost()) {
    Status("The host stopped or went out of reach.", true);
  } else {
    Status(others > 0 ? "Joined. You're linked." : "Joined.");
  }

  for (int i = 0; i < SIM_NET_MAX_PLAYERS; i++) {
    if (i == me || !(players[i].status == SIM_NET_PLAYERSTATUS_CLIENT || players[i].status == SIM_NET_PLAYERSTATUS_HOST)) {
      continue;
    }
    char name[SIM_NET_USERNAME_LEN + 1];
    snprintf(name, sizeof(name), "%s", players[i].userName);
    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle), "%s%s", players[i].status == SIM_NET_PLAYERSTATUS_HOST ? "Hosting" : "Joined",
             players[i].mpState != SIM_NET_MPSTATE_OFF ? ", using the wireless in the game" : "");
    ImGui::PushID(i);
    Card(name[0] != '\0' ? name : "Player", subtitle, false, false);
    ImGui::PopID();
  }

  const char *label = state == SIM_NET_STATE_HOST ? "Stop hosting" : "Leave";
  if (Button(label, "If the game is using the link right now, it shows a communication error.")) {
    SIM_Net_Stop();
    sStatus.clear();
  }
}

void DrawUnlinked() {
  if (SIM_Net_TakeConnectFailed()) {
    sStatus = "Couldn't join: no answer. Is the other device hosting, on the same network?";
    sStatusError = true;
  }
  const bool connecting = SIM_Net_IsConnecting() != FALSE;
  SIM_Net_SetSearching(connecting ? FALSE : TRUE);

  if (Button("Host", "This device waits for the other to join it.", false, !connecting)) {
    if (SIM_Net_StartHost()) {
      sStatus.clear();
    } else {
      sStatus = "Couldn't host: is another app using port 2009?";
      sStatusError = true;
    }
  }

  Section("Join a device that's hosting");
  if (connecting) {
    Progress(sStatus.c_str(), -1.0f);
    return;
  }
  SIM_Net_FoundHost found[SIM_NET_MAX_FOUND];
  const int count = SIM_Net_GetFoundHosts(found, SIM_NET_MAX_FOUND);
  if (count == 0) {
    Progress("Looking for devices hosting on this network...", -1.0f);
  }
  for (int i = 0; i < count; i++) {
    ImGui::PushID(100 + i);
    std::string subtitle = std::string(found[i].addressText) + ". Tap to join";
    if (Card(found[i].name[0] != '\0' ? found[i].name : found[i].addressText, subtitle.c_str())) {
      Join(found[i].addressText);
    }
    ImGui::PopID();
  }

  LoadAddress();
  char address[32];
  snprintf(address, sizeof(address), "%d.%d.%d.%d", sOctets[0], sOctets[1], sOctets[2], sOctets[3]);
  if (!sAddressOpen) {
    std::string title = std::string("Join by address: ") + address;
    if (Card(title.c_str(), "For a device that isn't found, e.g. on another network through a VPN. Tap to change it")) {
      sAddressOpen = true;
    }
  } else {
    static const char *const kParts[4] = { "Address, 1st number", "2nd number", "3rd number", "4th number" };
    for (int i = 0; i < 4; i++) {
      ImGui::PushID(200 + i);
      NumberField(kParts[i], nullptr, &sOctets[i], 0, 255);
      ImGui::PopID();
    }
    std::string label = std::string("Join ") + address;
    if (Button(label.c_str(), nullptr, true)) {
      sAddressOpen = false;
      Join(address);
    }
  }
  if (!sStatus.empty()) {
    Status(sStatus.c_str(), sStatusError);
  }
}

void DrawLinkPlay() {
  Note("Trade and battle with another device running this app, the way two DSs link up: one device hosts and the "
       "other joins. Both need to be on the same network (the same Wi-Fi, or a VPN such as Tailscale).");
  const char *hint = SIM_Prj_LinkPlayHint();
  if (hint != nullptr) {
    Note(hint);
  }

  const SIM_Net_State_t state = SIM_Net_GetCurrentState();
  if (state == SIM_NET_STATE_UNINITIALIZED) {
    Status("Not available in this build.", true);
    return;
  }
  if (state == SIM_NET_STATE_HOST || state == SIM_NET_STATE_CLIENT) {
    SIM_Net_SetSearching(FALSE);
    DrawLinked(state);
  } else {
    DrawUnlinked();
  }
}

} // namespace

void AddLinkPlayPage() {
  SIM_Net_SetDeviceName(SIM_DeviceName());
  AddPage("Link play", "Trade and battle with another device", DrawLinkPlay, 35);
}

void LinkPlayClosed() {
  SIM_Net_SetSearching(FALSE);
}

} // namespace SIM::Settings
