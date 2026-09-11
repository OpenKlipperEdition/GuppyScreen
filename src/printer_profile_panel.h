#ifndef __PRINTER_PROFILE_PANEL_H__
#define __PRINTER_PROFILE_PANEL_H__

#include "button_container.h"
#include "websocket_client.h"
#include "lvgl/lvgl.h"
#include "hv/json.hpp"

#include <string>
#include <vector>
#include <memory>
#include <functional>

using json = nlohmann::json;

struct PrinterProfileItem {
  std::string id;
  std::string name;
  std::string manufacturer;
  std::string kinematics;
  std::string description;
  std::string probe;
  std::string mcu;
  std::string photo_path;
  std::vector<int> bed_size;
  bool is_active{false};
  std::string path;
};

class PrinterProfilePanel {
 public:
  PrinterProfilePanel(KWebSocketClient &ws);
  ~PrinterProfilePanel();

  void foreground();
  void background();

  void refresh_profiles();
  void select_profile(const PrinterProfileItem &profile);

 private:
  static void _handle_callback(lv_event_t *event) {
    auto *panel = static_cast<PrinterProfilePanel*>(event->user_data);
    panel->handle_callback(event);
  }

  void handle_callback(lv_event_t *event);
  void build_profile_list();
  void show_switch_confirm(const PrinterProfileItem &profile);

  KWebSocketClient &ws;
  lv_obj_t *cont{nullptr};
  lv_obj_t *top_bar{nullptr};
  lv_obj_t *title_label{nullptr};
  lv_obj_t *list_cont{nullptr};
  ButtonContainer back_btn;

  std::vector<PrinterProfileItem> profiles;
  std::string active_profile_id;
  PrinterProfileItem pending_profile;
};

#endif // __PRINTER_PROFILE_PANEL_H__
