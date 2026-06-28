#pragma once

#include <string>

struct PlatformEvent {
    int type = 0;
    int xbutton_x = 0;
    int xbutton_y = 0;
    unsigned int xkey_state = 0;
    unsigned long xkey_keycode = 0;
    int xconfigure_width = 0;
    int xconfigure_height = 0;
    unsigned long selection_property = 0;
    unsigned long selection_target = 0;
    unsigned long selection_time = 0;
    unsigned long client_data = 0;
};

class IX11Backend {
public:
    virtual ~IX11Backend() = default;
    virtual bool poll_event(PlatformEvent& ev) = 0;
    virtual void request_redraw() = 0;
    virtual void present() = 0;
};
