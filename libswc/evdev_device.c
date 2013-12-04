#include "evdev_device.h"
#include "event.h"
#include "seat.h"
#include "util.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <libevdev/libevdev.h>

#define AXIS_STEP_DISTANCE 10
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

static inline uint32_t timeval_to_msec(struct timeval * time)
{
    return time->tv_sec * 1000 + time->tv_usec / 1000;
}

static void handle_key_event(struct swc_evdev_device * device,
                             struct input_event * input_event)
{
    uint32_t time = timeval_to_msec(&input_event->time);
    uint32_t state;

    if ((input_event->code >= BTN_MISC && input_event->code <= BTN_GEAR_UP)
        || input_event->code >= BTN_TRIGGER_HAPPY)
    {
        state = input_event->value ? WL_POINTER_BUTTON_STATE_PRESSED
                                   : WL_POINTER_BUTTON_STATE_RELEASED;
        device->handler->button(time, input_event->code, state);
    }
    else
    {
        state = input_event->value ? WL_KEYBOARD_KEY_STATE_PRESSED
                                   : WL_KEYBOARD_KEY_STATE_RELEASED;
        device->handler->key(time, input_event->code, state);
    }
}

static void handle_rel_event(struct swc_evdev_device * device,
                             struct input_event * input_event)
{
    uint32_t time = timeval_to_msec(&input_event->time);
    uint32_t axis, amount;

    switch (input_event->code)
    {
        case REL_X:
            device->motion.rel.dx += input_event->value;
            device->motion.rel.pending = true;
            return;
        case REL_Y:
            device->motion.rel.dy += input_event->value;
            device->motion.rel.pending = true;
            return;
        case REL_WHEEL:
            axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
            amount = -AXIS_STEP_DISTANCE * wl_fixed_from_int(input_event->value);
            break;
        case REL_HWHEEL:
            axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            amount = AXIS_STEP_DISTANCE * wl_fixed_from_int(input_event->value);
            break;
    }

    device->handler->axis(time, axis, amount);
}

static void handle_abs_event(struct swc_evdev_device * device,
                             struct input_event * input_event)
{
}

static void (* event_handlers[])(struct swc_evdev_device * device,
                                 struct input_event * input_event) = {
    [EV_KEY] = &handle_key_event,
    [EV_REL] = &handle_rel_event,
    [EV_ABS] = &handle_abs_event
};

static bool is_motion_event(struct input_event * event)
{
    return (event->type == EV_REL && (event->code == REL_X || event->code == REL_Y))
        || (event->type == EV_ABS && (event->code == ABS_X || event->code == ABS_Y));
}

static void handle_motion_events(struct swc_evdev_device * device,
                                 uint32_t time)
{
    if (device->motion.rel.pending)
    {
        wl_fixed_t dx = wl_fixed_from_int(device->motion.rel.dx);
        wl_fixed_t dy = wl_fixed_from_int(device->motion.rel.dy);

        device->handler->relative_motion(time, dx, dy);

        device->motion.rel.pending = false;
        device->motion.rel.dx = 0;
        device->motion.rel.dy = 0;
    }
}

static int handle_data(int fd, uint32_t mask, void * data)
{
    struct swc_evdev_device * device = data;
    struct input_event event;
    int ret;

    while (true)
    {
        ret = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL,
                                  &event);

        if (ret == -EAGAIN)
            break;

        while (ret == LIBEVDEV_READ_STATUS_SYNC)
        {
            ret = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_SYNC,
                                      &event);
        }

        if (!is_motion_event(&event))
            handle_motion_events(device, timeval_to_msec(&event.time));

        if (event.type < ARRAY_SIZE(event_handlers)
            && event_handlers[event.type])
        {
            event_handlers[event.type](device, &event);
        }
    }

    handle_motion_events(device, timeval_to_msec(&event.time));

    return 1;
}

struct swc_evdev_device * swc_evdev_device_new
    (const char * path, const struct swc_evdev_device_handler * handler)
{
    struct swc_evdev_device * device;

    if (!(device = malloc(sizeof *device)))
        goto error0;

    device->fd = swc_launch_open_device(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);

    if (device->fd == -1)
    {
        ERROR("Failed to open input device at %s\n", path);
        goto error0;
    }

    if (libevdev_new_from_fd(device->fd, &device->dev) != 0)
    {
        ERROR("Failed to create libevdev device\n");
        goto error1;
    }

    DEBUG("Adding device %s\n", libevdev_get_name(device->dev));

    device->handler = handler;
    device->capabilities = 0;
    memset(&device->motion, 0, sizeof device->motion);

    if (libevdev_has_event_code(device->dev, EV_KEY, KEY_ENTER))
    {
        device->capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
        DEBUG("\tThis device is a keyboard\n");
    }

    if (libevdev_has_event_code(device->dev, EV_REL, REL_X)
        && libevdev_has_event_code(device->dev, EV_REL, REL_Y)
        && libevdev_has_event_code(device->dev, EV_KEY, BTN_MOUSE))
    {
        device->capabilities |= WL_SEAT_CAPABILITY_POINTER;
        DEBUG("\tThis device is a pointer\n");
    }

    /* XXX: touch devices */

    return device;

  error1:
    close(device->fd);
  error0:
    return NULL;
}

void swc_evdev_device_destroy(struct swc_evdev_device * device)
{
    wl_event_source_remove(device->source);
    libevdev_free(device->dev);
    close(device->fd);
    free(device);
}

void swc_evdev_device_add_event_sources(struct swc_evdev_device * device,
                                        struct wl_event_loop * event_loop)
{
    DEBUG("Adding event source for %s\n", libevdev_get_name(device->dev));
    device->source
        = wl_event_loop_add_fd(event_loop, device->fd, WL_EVENT_READABLE,
                               handle_data, device);
}

