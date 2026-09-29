#include "setting_panel.h"
#include "state.h"
#include "utils.h"
#include "config.h"
#include "spdlog/spdlog.h"
#include "subprocess.hpp"

#include <sys/reboot.h>
#include <unistd.h>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;
namespace sp = subprocess;

LV_IMG_DECLARE(network_img);
LV_IMG_DECLARE(refresh_img);
LV_IMG_DECLARE(spoolman_img);
LV_IMG_DECLARE(update_img);
LV_IMG_DECLARE(power_devices_img);

#ifdef ZBOLT
LV_IMG_DECLARE(info_img);
#else
LV_IMG_DECLARE(sysinfo_img);
#endif

LV_IMG_DECLARE(print);

SettingPanel::SettingPanel(KWebSocketClient &c, std::mutex &l, lv_obj_t *parent, SpoolmanPanel &sm)
  : ws(c)
  , cont(lv_obj_create(parent))
#ifndef OS_ANDROID
  , wifi_panel(l)
#endif
  , sysinfo_panel()
  , spoolman_panel(sm)
  , printer_profile_panel(c)
  , update_panel(c)
  , wifi_btn(cont, &network_img, "WIFI", &SettingPanel::_handle_callback, this)
  , printer_profile_btn(cont, &print, "Printer\nModel", &SettingPanel::_handle_callback, this)
  , spoolman_btn(cont, &spoolman_img, "Spoolman", &SettingPanel::_handle_callback, this)
  , guppy_update_btn(cont, &update_img, "System\nUpdate", &SettingPanel::_handle_callback, this)
#ifdef ZBOLT
  , sysinfo_btn(cont, &info_img, "System", &SettingPanel::_handle_callback, this)
#else
  , sysinfo_btn(cont, &sysinfo_img, "System", &SettingPanel::_handle_callback, this)
#endif
  , power_btn(cont, &power_devices_img, "Power", &SettingPanel::_handle_callback, this)
{
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));

  spoolman_btn.disable();
#ifdef OS_ANDROID
  wifi_btn.disable();
#endif

  static lv_coord_t grid_main_row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(5), LV_GRID_FR(5), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST};
  static lv_coord_t grid_main_col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST};

  lv_obj_set_grid_dsc_array(cont, grid_main_col_dsc, grid_main_row_dsc);

  // Row 1
  lv_obj_set_grid_cell(wifi_btn.get_container(), LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
  lv_obj_set_grid_cell(printer_profile_btn.get_container(), LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
  lv_obj_set_grid_cell(guppy_update_btn.get_container(), LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 1, 1);

  // Row 2
  lv_obj_set_grid_cell(spoolman_btn.get_container(), LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
  lv_obj_set_grid_cell(sysinfo_btn.get_container(), LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
  lv_obj_set_grid_cell(power_btn.get_container(), LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
}

SettingPanel::~SettingPanel() {
  if (cont != NULL) {
    lv_obj_del(cont);
    cont = NULL;
  }
}

lv_obj_t *SettingPanel::get_container() {
  return cont;
}

void SettingPanel::handle_callback(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_obj_t *btn = lv_event_get_current_target(event);

    if (btn == wifi_btn.get_container()) {
      spdlog::trace("wifi pressed");
#ifndef OS_ANDROID
      wifi_panel.foreground();
#endif
    } else if (btn == sysinfo_btn.get_container()) {
      spdlog::trace("setting system info pressed");
      sysinfo_panel.foreground();
    } else if (btn == spoolman_btn.get_container()) {
      spdlog::trace("setting spoolman pressed");
      spoolman_panel.foreground();
    } else if (btn == guppy_update_btn.get_container()) {
      spdlog::trace("setting system update pressed");
      update_panel.foreground();
    } else if (btn == printer_profile_btn.get_container()) {
      spdlog::trace("setting printer profile pressed");
      printer_profile_panel.foreground();
    } else if (btn == power_btn.get_container()) {
      spdlog::trace("setting power pressed");
      if (KUtils::is_printing()) {
        show_safety_alert(
          "Printing in Progress",
          "Cannot perform power or restart actions while a print is active or paused.\n\nPlease cancel or wait for the print to complete."
        );
        return;
      }
      show_power_dialog();
    }
  }
}

void SettingPanel::show_power_dialog() {
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_pad_all(overlay, 0, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

  // Touching the backdrop closes the power dialog
  lv_obj_add_event_cb(overlay, [](lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *current_target = lv_event_get_current_target(e);
    if (target == current_target) {
      lv_obj_del_async(current_target);
    }
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *box = lv_obj_create(overlay);
  KUtils::style_dialog_box(box);
  lv_obj_set_size(box, LV_PCT(92), LV_PCT(90));
  lv_obj_center(box);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(box, 6, 0);
  lv_obj_set_style_pad_all(box, 10, 0);

  lv_obj_t *title = lv_label_create(box);
  KUtils::style_dialog_title(title);
  lv_label_set_text(title, "Power & Restart");
  lv_obj_set_width(title, LV_PCT(100));

  // Scrollable container for options
  lv_obj_t *scroll_cont = lv_obj_create(box);
  lv_obj_set_size(scroll_cont, LV_PCT(100), LV_PCT(74));
  lv_obj_set_flex_flow(scroll_cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(scroll_cont, 2, 0);
  lv_obj_set_style_pad_gap(scroll_cont, 6, 0);
  lv_obj_add_flag(scroll_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(scroll_cont, 0, 0);

  auto make_opt_btn = [&](const char *lbl_text, const char *desc_text, lv_palette_t palette) -> lv_obj_t * {
    lv_obj_t *btn = lv_btn_create(scroll_cont);
    lv_obj_set_size(btn, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, lv_palette_darken(palette, 2), 0);
    lv_obj_set_style_pad_all(btn, 8, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(btn, 2, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, lbl_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl, LV_PCT(100));

    lv_obj_t *desc = lv_label_create(btn);
    lv_label_set_text(desc, desc_text);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(desc, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_width(desc, LV_PCT(100));
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);

    return btn;
  };

  // Options:
  // 1. Restart Klipper
  lv_obj_t *klipper_btn = make_opt_btn(
    "Restart Klipper",
    "Restarts the Klipper host software service (RESTART).",
    LV_PALETTE_GREY
  );

  // 2. Restart Firmware (MCU)
  lv_obj_t *mcu_btn = make_opt_btn(
    "Restart Firmware (MCU)",
    "Resets and reconnects all printer microcontrollers (FIRMWARE_RESTART).",
    LV_PALETTE_GREY
  );

  // 3. Restart GuppyScreen
  lv_obj_t *guppy_btn = make_opt_btn(
    "Restart GuppyScreen",
    "Restarts the display user interface without touching Klipper.",
    LV_PALETTE_GREY
  );

  // 4. Reboot System
  lv_obj_t *reboot_btn = make_opt_btn(
    "Reboot System",
    "Safely syncs storage and reboots the entire Linux system.",
    LV_PALETTE_GREY
  );

  // Close button
  lv_obj_t *close_btn = lv_btn_create(box);
  lv_obj_set_size(close_btn, 140, 36);
  lv_obj_set_style_bg_color(close_btn, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
  lv_obj_t *close_lbl = lv_label_create(close_btn);
  lv_label_set_text(close_lbl, "Cancel");
  lv_obj_center(close_lbl);

  lv_obj_add_event_cb(close_btn, [](lv_event_t *e) {
    lv_obj_del_async(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target(e))));
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_add_event_cb(klipper_btn, [](lv_event_t *e) {
    auto *self = static_cast<SettingPanel*>(e->user_data);
    lv_obj_del_async(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target(e)))));
    if (KUtils::is_printing()) {
      self->show_safety_alert(
        "Cannot Restart Klipper",
        "A print job is currently active or paused.\n\nPlease cancel or wait for the print to complete."
      );
      return;
    }
    auto s = State::get_instance();
    auto etarget_j = s->get_data("/printer_state/extruder/target"_json_pointer);
    auto btarget_j = s->get_data("/printer_state/heater_bed/target"_json_pointer);
    double etarget = etarget_j.is_number() ? etarget_j.template get<double>() : 0.0;
    double btarget = btarget_j.is_number() ? btarget_j.template get<double>() : 0.0;
    if (etarget > 0.0 || btarget > 0.0) {
      self->show_confirm(
        "Heaters Still Active!",
        "Restarting Klipper will shut down active heating.\n\nAre you sure you want to restart Klipper now?",
        [self]() {
          spdlog::info("Restarting Klipper from power dialog");
          self->ws.send_jsonrpc("printer.restart");
        }
      );
    } else {
      self->show_confirm(
        "Restart Klipper?",
        "Restarts the Klipper host software service.\n\nAre you sure you want to restart Klipper?",
        [self]() {
          spdlog::info("Restarting Klipper from power dialog");
          self->ws.send_jsonrpc("printer.restart");
        }
      );
    }
  }, LV_EVENT_CLICKED, this);

  lv_obj_add_event_cb(mcu_btn, [](lv_event_t *e) {
    auto *self = static_cast<SettingPanel*>(e->user_data);
    lv_obj_del_async(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target(e)))));
    if (KUtils::is_printing()) {
      self->show_safety_alert(
        "Cannot Restart Firmware",
        "A print job is currently active or paused.\n\nPlease cancel or wait for the print to complete."
      );
      return;
    }
    auto s = State::get_instance();
    auto etarget_j = s->get_data("/printer_state/extruder/target"_json_pointer);
    auto btarget_j = s->get_data("/printer_state/heater_bed/target"_json_pointer);
    double etarget = etarget_j.is_number() ? etarget_j.template get<double>() : 0.0;
    double btarget = btarget_j.is_number() ? btarget_j.template get<double>() : 0.0;
    if (etarget > 0.0 || btarget > 0.0) {
      self->show_confirm(
        "Heaters Still Active!",
        "Resetting firmware will immediately shut down active heating.\n\nAre you sure you want to restart firmware now?",
        [self]() {
          spdlog::info("Restarting Firmware from power dialog");
          self->ws.send_jsonrpc("printer.firmware_restart");
        }
      );
    } else {
      self->show_confirm(
        "Restart Firmware?",
        "Resets and reconnects all printer microcontrollers.\n\nAre you sure you want to restart firmware?",
        [self]() {
          spdlog::info("Restarting Firmware from power dialog");
          self->ws.send_jsonrpc("printer.firmware_restart");
        }
      );
    }
  }, LV_EVENT_CLICKED, this);

  lv_obj_add_event_cb(guppy_btn, [](lv_event_t *e) {
    auto *self = static_cast<SettingPanel*>(e->user_data);
    lv_obj_del_async(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target(e)))));
    self->show_confirm(
      "Restart GuppyScreen?",
      "Restarts the display user interface without touching Klipper.\n\nAre you sure you want to restart GuppyScreen?",
      []() {
        spdlog::info("Restarting GuppyScreen from power dialog");
        KUtils::restart_guppyscreen();
      }
    );
  }, LV_EVENT_CLICKED, this);

  lv_obj_add_event_cb(reboot_btn, [](lv_event_t *e) {
    auto *self = static_cast<SettingPanel*>(e->user_data);
    lv_obj_del_async(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target(e)))));
    self->request_reboot();
  }, LV_EVENT_CLICKED, this);
}

void SettingPanel::request_reboot() {
  if (KUtils::is_printing()) {
    show_safety_alert(
      "Cannot Reboot",
      "A print job is currently active or paused.\n\nPlease cancel or wait for the print to complete before rebooting."
    );
    return;
  }

  auto s = State::get_instance();
  auto etarget_j = s->get_data("/printer_state/extruder/target"_json_pointer);
  auto etemp_j   = s->get_data("/printer_state/extruder/temperature"_json_pointer);
  auto btarget_j = s->get_data("/printer_state/heater_bed/target"_json_pointer);
  auto btemp_j   = s->get_data("/printer_state/heater_bed/temperature"_json_pointer);

  double etarget = etarget_j.is_number() ? etarget_j.template get<double>() : 0.0;
  double etemp   = etemp_j.is_number()   ? etemp_j.template get<double>()   : 0.0;
  double btarget = btarget_j.is_number() ? btarget_j.template get<double>() : 0.0;
  double btemp   = btemp_j.is_number()   ? btemp_j.template get<double>()   : 0.0;

  if (etarget > 0.0 || btarget > 0.0) {
    std::string heater_info;
    if (etarget > 0.0 && btarget > 0.0) {
      heater_info = fmt::format("Extruder: {:.0f}/{:.0f}°C\nBed: {:.0f}/{:.0f}°C", etemp, etarget, btemp, btarget);
    } else if (etarget > 0.0) {
      heater_info = fmt::format("Extruder heating: {:.0f}/{:.0f}°C", etemp, etarget);
    } else {
      heater_info = fmt::format("Bed heating: {:.0f}/{:.0f}°C", btemp, btarget);
    }
    std::string prompt = fmt::format("{}\n\nRebooting while heaters are active may cause heat creep or nozzle clogging.\n\nAre you sure you want to reboot now?", heater_info);
    show_confirm(
      "Heaters Still Active!",
      prompt.c_str(),
      []() {
        spdlog::info("Executing system reboot");
        sync();
        int rc = system("sync; reboot -f || /sbin/reboot -f || reboot || /sbin/reboot");
        (void)rc;
        reboot(RB_AUTOBOOT);
      }
    );
    return;
  }

  if (etemp > 50.0) {
    std::string hotend_prompt = fmt::format("Extruder temperature is currently {:.0f}°C.\n\nRebooting stops the heatsink fan temporarily. Reboot anyway?", etemp);
    show_confirm(
      "Hotend Still Hot!",
      hotend_prompt.c_str(),
      []() {
        spdlog::info("Executing system reboot");
        sync();
        int rc = system("sync; reboot -f || /sbin/reboot -f || reboot || /sbin/reboot");
        (void)rc;
        reboot(RB_AUTOBOOT);
      }
    );
    return;
  }

  show_confirm(
    "Reboot System?",
    "Rebooting will restart all printer services and the Linux system.\n\nAre you sure you want to reboot now?",
    []() {
      spdlog::info("Executing system reboot");
      sync();
      int rc = system("sync; reboot -f || /sbin/reboot -f || reboot || /sbin/reboot");
      (void)rc;
      reboot(RB_AUTOBOOT);
    }
  );
}

void SettingPanel::show_safety_alert(const char *title, const std::string &detail) {
  static const char *btns[] = {"OK", ""};

  std::string msg_str = fmt::format("{}\n\n{}", title, detail);
  lv_obj_t *mbox = lv_msgbox_create(NULL, NULL, msg_str.c_str(), btns, false);
  KUtils::style_dialog_msgbox(mbox);

  lv_obj_t *msg = ((lv_msgbox_t *)mbox)->text;
  lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(msg, LV_PCT(100));
  lv_obj_center(msg);

  lv_obj_t *btnm = lv_msgbox_get_btns(mbox);
  lv_obj_add_flag(btnm, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, 0);

  auto hscale = (double)lv_disp_get_physical_ver_res(NULL) / 480.0;
  lv_obj_set_size(btnm, LV_PCT(50), 50 * hscale);
  lv_obj_set_size(mbox, LV_PCT(80), LV_PCT(65));
  lv_obj_center(mbox);

  lv_obj_add_event_cb(btnm, [](lv_event_t *e) {
    lv_msgbox_close(lv_obj_get_parent(lv_event_get_current_target(e)));
  }, LV_EVENT_VALUE_CHANGED, NULL);
}

void SettingPanel::show_confirm(const char *title, const char *detail,
                                const std::function<void()> &cb) {
  static const char *btns[] = {"Cancel", "Confirm", ""};

  std::string msg_str = fmt::format("{}\n\n{}", title, detail);
  lv_obj_t *mbox = lv_msgbox_create(NULL, NULL, msg_str.c_str(), btns, false);
  KUtils::style_dialog_msgbox(mbox);

  lv_obj_t *msg = ((lv_msgbox_t *)mbox)->text;
  lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(msg, LV_PCT(100));
  lv_obj_center(msg);

  lv_obj_t *btnm = lv_msgbox_get_btns(mbox);
  lv_btnmatrix_set_btn_ctrl(btnm, 0, LV_BTNMATRIX_CTRL_CHECKED);
  lv_btnmatrix_set_btn_ctrl(btnm, 1, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_flag(btnm, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, 0);

  auto hscale = (double)lv_disp_get_physical_ver_res(NULL) / 480.0;
  lv_obj_set_size(btnm, LV_PCT(90), 50 * hscale);
  lv_obj_set_size(mbox, LV_PCT(80), LV_PCT(65));
  lv_obj_center(mbox);

  lv_obj_add_event_cb(btnm, [](lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part == LV_PART_ITEMS && dsc->id == 1) {
      dsc->rect_dsc->bg_color = lv_palette_main(LV_PALETTE_RED);
    }
  }, LV_EVENT_DRAW_PART_BEGIN, NULL);

  auto *cb_holder = new std::function<void()>(cb);
  lv_obj_add_event_cb(btnm, [](lv_event_t *e) {
    auto *holder = (std::function<void()> *)e->user_data;
    uint16_t btn_id = lv_btnmatrix_get_selected_btn(lv_event_get_current_target(e));
    lv_obj_t *mbox = lv_obj_get_parent(lv_event_get_current_target(e));
    lv_msgbox_close(mbox);
    if (btn_id == 1 && holder && *holder) {
      (*holder)();
    }
    delete holder;
  }, LV_EVENT_VALUE_CHANGED, cb_holder);
}

void SettingPanel::enable_spoolman() {
  spoolman_btn.enable();
}

