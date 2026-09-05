#ifndef GUI_H
#define GUI_H

#ifdef __cplusplus
extern "C" {
#endif

void run_gui(void);
bool gui_can_launch(void);
void gui_shutdown(void);


void gui_move_selection(int direction);
void gui_refresh_files(void);
bool gui_handle_touch(int x, int y);
const char* gui_get_selected_app_path(void);

#ifdef __cplusplus
}
#endif

#endif
