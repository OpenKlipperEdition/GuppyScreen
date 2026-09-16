#ifndef __UPDATE_PANEL_H__
#define __UPDATE_PANEL_H__

#include "button_container.h"
#include "websocket_client.h"
#include "lvgl/lvgl.h"

#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <atomic>
#include <thread>
#include <ctime>

struct UpdatePackageItem {
  std::string file_path;
  std::string file_name;
  std::string file_size;
  std::string location_tag;
  std::string version;
  std::string status_badge;
  int version_diff{0}; // >0 newer, 0 same, <0 older
  time_t modified_time{0};
};

enum class UpdateState {
  IDLE,
  CONFIRMING,
  FLASHING,
  SUCCESS,
  FAILED
};

class UpdatePanel {
 public:
  UpdatePanel(KWebSocketClient &ws);
  ~UpdatePanel();

  void foreground();
  void background();

  void scan_updates();
  void start_update(const UpdatePackageItem &pkg);

 private:
  static void _handle_callback(lv_event_t *event) {
    auto *panel = static_cast<UpdatePanel*>(event->user_data);
    panel->handle_callback(event);
  }

  static void _timer_callback(lv_timer_t *timer) {
    auto *panel = static_cast<UpdatePanel*>(timer->user_data);
    panel->timer_tick();
  }

  void handle_callback(lv_event_t *event);
  void timer_tick();
  void check_usb_auto_detect();
  void show_usb_detect_popup(const UpdatePackageItem &pkg);
  void close_usb_detect_popup();
  void build_package_list();
  void show_confirmation_modal(const UpdatePackageItem &pkg);
  void show_progress_view(const UpdatePackageItem &pkg);
  void close_modal();
  void execute_update_thread(std::string swu_path);

  KWebSocketClient &ws;
  lv_obj_t *cont{nullptr};
  lv_obj_t *top_bar{nullptr};
  lv_obj_t *title_label{nullptr};
  lv_obj_t *list_cont{nullptr};
  lv_obj_t *modal_cont{nullptr};
  lv_obj_t *usb_detect_mbox{nullptr};
  lv_obj_t *progress_bar{nullptr};
  lv_obj_t *progress_label{nullptr};
  lv_obj_t *status_label{nullptr};
  lv_obj_t *reboot_btn{nullptr};
  ButtonContainer back_btn;
  ButtonContainer scan_btn;

  lv_timer_t *update_timer{nullptr};
  int scan_tick_counter{0};

  std::vector<UpdatePackageItem> found_packages;
  std::set<std::string> prompted_packages;
  UpdatePackageItem selected_package;
  UpdatePackageItem pending_usb_package;

  std::atomic<UpdateState> state{UpdateState::IDLE};
  std::atomic<int> progress_percent{0};
  std::atomic<int> current_step{0};
  std::atomic<int> total_steps{0};
  std::string status_message;
  std::string error_message;
  std::mutex status_mutex;
  std::thread worker_thread;
};

#endif // __UPDATE_PANEL_H__
