#ifndef __APPSTORE_PANEL_H__
#define __APPSTORE_PANEL_H__

#include "button_container.h"
#include "websocket_client.h"
#include "lvgl/lvgl.h"
#include "hv/json.hpp"

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

using json = nlohmann::json;

struct AppItem {
  std::string id;
  std::string name;
  std::string category;
  std::string icon;
  std::string version;
  std::string author;
  std::string description;
  bool is_builtin{false};
  bool is_installed{false};
  bool is_active{false};
  std::string status; // "active", "installed", "available"
};

class AppStorePanel {
 public:
  AppStorePanel(KWebSocketClient &ws);
  ~AppStorePanel();

  void foreground();
  void background();

  void refresh_apps();
  void filter_category(const std::string &category);
  void execute_refresh();
  void execute_install(const AppItem &app, bool activate = false);
  void execute_activate(const AppItem &app);
  void execute_remove(const AppItem &app);

 private:
  static void _handle_callback(lv_event_t *event) {
    auto *panel = static_cast<AppStorePanel*>(event->user_data);
    panel->handle_callback(event);
  }

  static void _timer_callback(lv_timer_t *timer) {
    auto *panel = static_cast<AppStorePanel*>(timer->user_data);
    panel->check_async_task();
  }

  void handle_callback(lv_event_t *event);
  void build_category_bar();
  void build_app_list();
  void show_install_confirm(const AppItem &app);
  void show_activate_confirm(const AppItem &app);
  void show_remove_confirm(const AppItem &app);
  void show_progress_modal(const std::string &title, const std::string &msg);
  void close_modal();
  void show_alert(const std::string &title, const std::string &msg);
  void check_async_task();

  KWebSocketClient &ws;
  lv_obj_t *cont{nullptr};
  lv_obj_t *top_bar{nullptr};
  lv_obj_t *title_label{nullptr};
  lv_obj_t *category_cont{nullptr};
  lv_obj_t *list_cont{nullptr};
  ButtonContainer back_btn;
  ButtonContainer refresh_btn;

  std::vector<AppItem> apps;
  std::string current_category{"all"};
  AppItem pending_app;

  // Async task tracking
  std::thread worker_thread;
  std::atomic<bool> is_busy{false};
  std::atomic<bool> task_finished{false};
  std::atomic<int> task_exit_code{0};
  std::string task_status_msg;
  std::string task_error_msg;
  lv_obj_t *modal_box{nullptr};
  lv_timer_t *poll_timer{nullptr};
};

#endif // __APPSTORE_PANEL_H__
