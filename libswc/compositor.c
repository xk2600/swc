#include <stdlib.h>
#include <stdio.h>
#include <libudev.h>
#include <gbm.h>

#include "compositor.h"
#include "compositor_surface.h"
#include "tty.h"
#include "output.h"
#include "surface.h"
#include "event.h"
#include "region.h"
#include "data_device_manager.h"
#include "util.h"

static const char default_seat[] = "seat0";

static void calculate_damage(struct swc_compositor * compositor)
{
    struct swc_surface * surface;
    struct swc_compositor_surface_state * state;
    pixman_region32_t opaque, surface_opaque;

    pixman_region32_clear(&compositor->opaque);
    pixman_region32_init(&surface_opaque);

    /* Go through surfaces top-down to calculate clipping regions. */
    wl_list_for_each(surface, &compositor->surfaces, link)
    {
        state = surface->class_state;

        /* Clip the surface by the opaque region covering it. */
        pixman_region32_copy(&state->clip, &compositor->opaque);

        /* Translate the opaque region to global coordinates. */
        pixman_region32_copy(&surface_opaque, &surface->state.opaque);
        pixman_region32_translate(&surface_opaque, surface->geometry.x,
                                  surface->geometry.y);

        /* Add the surface's opaque region to the accumulated opaque
         * region. */
        pixman_region32_union(&compositor->opaque, &compositor->opaque,
                              &surface_opaque);

        if (pixman_region32_not_empty(&surface->state.damage))
        {
            swc_renderer_flush(&compositor->renderer, surface);

            /* Translate surface damage to global coordinates. */
            pixman_region32_translate(&surface->state.damage,
                                      surface->geometry.x,
                                      surface->geometry.y);

            /* Add the surface damage to the compositor damage. */
            pixman_region32_union(&compositor->damage, &compositor->damage,
                                  &surface->state.damage);
            pixman_region32_clear(&surface->state.damage);
        }

        if (state->border.damaged)
        {
            pixman_region32_t border_region, surface_region;

            pixman_region32_init_with_extents(&border_region, &state->extents);
            pixman_region32_init_rect
                (&surface_region, surface->geometry.x, surface->geometry.y,
                 surface->geometry.width, surface->geometry.height);

            pixman_region32_subtract(&border_region, &border_region,
                                     &surface_region);

            pixman_region32_union(&compositor->damage, &compositor->damage,
                                  &border_region);

            pixman_region32_fini(&border_region);
            pixman_region32_fini(&surface_region);

            state->border.damaged = false;
        }
    }

    pixman_region32_fini(&surface_opaque);
}

static void repaint_output(struct swc_compositor * compositor,
                           struct swc_output * output)
{
    pixman_region32_t damage, previous_damage, base_damage;

    pixman_region32_init(&damage);
    pixman_region32_init(&previous_damage);
    pixman_region32_init(&base_damage);

    pixman_region32_intersect_rect
        (&damage, &compositor->damage, output->geometry.x, output->geometry.y,
         output->geometry.width, output->geometry.height);

    /* We must save the damage from the previous frame because the back buffer
     * is also damaged in this region. */
    pixman_region32_copy(&previous_damage, &output->previous_damage);
    pixman_region32_copy(&output->previous_damage, &damage);

    /* The total damage is composed of the damage from the new frame, and the
     * damage from the last frame. */
    pixman_region32_union(&damage, &damage, &previous_damage);

    pixman_region32_subtract(&base_damage, &damage, &compositor->opaque);

    swc_renderer_set_target(&compositor->renderer, &output->framebuffer_plane);
    swc_renderer_repaint(&compositor->renderer, &damage, &base_damage,
                         &compositor->surfaces);

    pixman_region32_subtract(&compositor->damage, &compositor->damage, &damage);

    pixman_region32_fini(&damage);
    pixman_region32_fini(&previous_damage);
    pixman_region32_fini(&base_damage);

    if (!swc_plane_flip(&output->framebuffer_plane))
        fprintf(stderr, "Plane flip failed\n");
}

static void perform_update(void * data)
{
    struct swc_compositor * compositor = data;
    struct swc_output * output;
    uint32_t updates = compositor->scheduled_updates
                       & ~compositor->pending_flips;

    if (updates)
    {
        printf("performing update\n");
        calculate_damage(compositor);

        wl_list_for_each(output, &compositor->outputs, link)
        {
            if (updates & SWC_OUTPUT_MASK(output))
                repaint_output(compositor, output);
        }

        compositor->pending_flips |= updates;
        compositor->scheduled_updates &= ~updates;
    }

}

static bool handle_key(struct swc_keyboard * keyboard, uint32_t time,
                       uint32_t key, uint32_t state)
{
    struct swc_seat * seat;
    struct swc_binding * binding;
    struct swc_compositor * compositor;
    char keysym_name[64];

    seat = swc_container_of(keyboard, typeof(*seat), keyboard);
    compositor = swc_container_of(seat, typeof(*compositor), seat);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        xkb_keysym_t keysym;

        keysym = xkb_state_key_get_one_sym(seat->xkb.state, key + 8);

        wl_array_for_each(binding, &compositor->key_bindings)
        {
            if (binding->value == keysym)
            {
                xkb_mod_mask_t mod_mask;
                uint32_t modifiers = 0;
                mod_mask = xkb_state_serialize_mods(seat->xkb.state,
                                                    XKB_STATE_MODS_EFFECTIVE);
                mod_mask = xkb_state_mod_mask_remove_consumed(seat->xkb.state, key + 8,
                                                              mod_mask);

                if (mod_mask & (1 << seat->xkb.indices.ctrl))
                    modifiers |= SWC_MOD_CTRL;
                if (mod_mask & (1 << seat->xkb.indices.alt))
                    modifiers |= SWC_MOD_ALT;
                if (mod_mask & (1 << seat->xkb.indices.super))
                    modifiers |= SWC_MOD_LOGO;
                if (mod_mask & (1 << seat->xkb.indices.shift))
                    modifiers |= SWC_MOD_SHIFT;

                if (binding->modifiers == SWC_MOD_ANY
                    || binding->modifiers == modifiers)
                {
                    binding->handler(time, keysym, binding->data);
                    printf("\t-> handled\n");
                    return true;
                }
            }
        }
    }

    return false;
}

struct swc_keyboard_handler keyboard_handler = {
    .key = &handle_key,
};

static void handle_focus(struct swc_pointer * pointer)
{
    struct swc_seat * seat;
    struct swc_compositor * compositor;
    struct swc_surface * surface;
    int32_t surface_x, surface_y;

    seat = swc_container_of(pointer, typeof(*seat), pointer);
    compositor = swc_container_of(seat, typeof(*compositor), seat);

    wl_list_for_each(surface, &compositor->surfaces, link)
    {
        pixman_region32_t region;

        pixman_region32_init_rect
            (&region, surface->geometry.x, surface->geometry.y,
             surface->geometry.width, surface->geometry.height);

        surface_x = wl_fixed_to_int(pointer->x) - surface->geometry.x;
        surface_y = wl_fixed_to_int(pointer->y) - surface->geometry.y;

        if (pixman_region32_contains_point(&surface->state.input,
                                           surface_x, surface_y, NULL))
        {
            swc_pointer_set_focus(pointer, surface);
            return;
        }
    }

    swc_pointer_set_focus(pointer, NULL);
}

static bool handle_motion(struct swc_pointer * pointer, uint32_t time)
{
    struct swc_seat * seat;
    struct swc_compositor * compositor;

    seat = swc_container_of(pointer, typeof(*seat), pointer);
    compositor = swc_container_of(seat, typeof(*compositor), seat);

    return false;
}

struct swc_pointer_handler pointer_handler = {
    .focus = &handle_focus,
    .motion = &handle_motion
};

/* XXX: maybe this should go in swc_drm */
static void handle_tty_event(struct wl_listener * listener, void * data)
{
    struct swc_event * event = data;
    struct swc_compositor * compositor;

    compositor = swc_container_of(listener, typeof(*compositor), tty_listener);

    switch (event->type)
    {
        case SWC_TTY_VT_ENTER:
            swc_drm_set_master(&compositor->drm);
            break;
        case SWC_TTY_VT_LEAVE:
            swc_drm_drop_master(&compositor->drm);
            break;
    }
}

static void handle_drm_event(struct wl_listener * listener, void * data)
{
    struct swc_event * event = data;
    struct swc_compositor * compositor;

    compositor = swc_container_of(listener, typeof(*compositor), drm_listener);

    switch (event->type)
    {
        case SWC_DRM_PAGE_FLIP:
        {
            struct swc_drm_event_data * event_data = event->data;
            struct swc_surface * surface;

            compositor->pending_flips &= ~SWC_OUTPUT_MASK(event_data->output);

            if (compositor->pending_flips == 0)
            {
                wl_list_for_each(surface, &compositor->surfaces, link)
                    swc_surface_send_frame_callbacks(surface, event_data->time);
            }

            /* If we had scheduled updates that couldn't run because we were
             * waiting on a page flip, run them now. */
            if (compositor->scheduled_updates)
                perform_update(compositor);

            break;
        }
    }
}

static void handle_surface_destroy(struct wl_listener * listener, void * data)
{
    struct wl_resource * resource = data;
    struct swc_surface * surface = wl_resource_get_user_data(resource);

    wl_list_remove(&surface->link);

    free(surface);
}

static void handle_terminate(uint32_t time, uint32_t value, void * data)
{
    struct wl_display * display = data;
    printf("handling terminate\n");
    wl_display_terminate(display);
}

static void handle_switch_vt(uint32_t time, uint32_t value, void * data)
{
    struct swc_tty * tty = data;
    uint8_t vt = value - XKB_KEY_XF86Switch_VT_1 + 1;
    printf("handle switch vt%u\n", vt);
    if (vt != tty->vt)
        swc_tty_switch_vt(tty, vt);
}

static void create_surface(struct wl_client * client,
                           struct wl_resource * resource, uint32_t id)
{
    struct swc_compositor * compositor = wl_resource_get_user_data(resource);
    struct swc_surface * surface;
    struct swc_output * output;

    printf("compositor.create_surface\n");

    output = swc_container_of(compositor->outputs.next, typeof(*output), link);

    /* Initialize surface. */
    surface = swc_surface_new(client, id);

    if (!surface)
    {
        wl_resource_post_no_memory(resource);
        return;
    }
}

static void create_region(struct wl_client * client,
                          struct wl_resource * resource, uint32_t id)
{
    struct swc_region * region;

    region = swc_region_new(client, id);

    if (!region)
        wl_resource_post_no_memory(resource);
}

struct wl_compositor_interface compositor_implementation = {
    .create_surface = &create_surface,
    .create_region = &create_region
};

static void bind_compositor(struct wl_client * client, void * data,
                            uint32_t version, uint32_t id)
{
    struct swc_compositor * compositor = data;
    struct wl_resource * resource;

    if (version >= 3)
        version = 3;

    resource = wl_resource_create(client, &wl_compositor_interface,
                                  version, id);
    wl_resource_set_implementation(resource, &compositor_implementation,
                                   compositor, NULL);
}

bool swc_compositor_initialize(struct swc_compositor * compositor,
                               struct wl_display * display)
{
    struct wl_event_loop * event_loop;
    struct udev_device * drm_device;
    struct wl_list * outputs;
    struct swc_output * output;
    pixman_region32_t pointer_region;
    xkb_keysym_t keysym;

    compositor->display = display;
    compositor->tty_listener.notify = &handle_tty_event;
    compositor->drm_listener.notify = &handle_drm_event;
    compositor->scheduled_updates = 0;
    compositor->pending_flips = 0;
    compositor->compositor_class.interface
        = &swc_compositor_class_implementation;

    compositor->udev = udev_new();

    if (compositor->udev == NULL)
    {
        printf("could not initialize udev context\n");
        goto error_base;
    }

    event_loop = wl_display_get_event_loop(display);

    if (!swc_tty_initialize(&compositor->tty, event_loop, 2))
    {
        printf("could not initialize tty\n");
        goto error_udev;
    }

    wl_signal_add(&compositor->tty.event_signal, &compositor->tty_listener);

    /* TODO: configurable seat */
    if (!swc_seat_initialize(&compositor->seat, compositor->udev,
                             default_seat))
    {
        printf("could not initialize seat\n");
        goto error_tty;
    }

    swc_seat_add_event_sources(&compositor->seat, event_loop);
    compositor->seat.keyboard.handler = &keyboard_handler;
    compositor->seat.pointer.handler = &pointer_handler;

    /* TODO: configurable seat */
    if (!swc_drm_initialize(&compositor->drm, compositor->udev, default_seat))
    {
        printf("could not initialize drm\n");
        goto error_seat;
    }

    wl_signal_add(&compositor->drm.event_signal, &compositor->drm_listener);
    swc_drm_add_event_sources(&compositor->drm, event_loop);

    if (!swc_renderer_initialize(&compositor->renderer, &compositor->drm))
    {
        printf("could not initialize renderer\n");
        goto error_drm;
    }

    outputs = swc_drm_create_outputs(&compositor->drm);

    if (outputs)
    {
        wl_list_init(&compositor->outputs);
        wl_list_insert_list(&compositor->outputs, outputs);
        free(outputs);
    }
    else
    {
        printf("could not create outputs\n");
        goto error_renderer;
    }

    /* Calculate pointer region */
    pixman_region32_init(&pointer_region);

    wl_list_for_each(output, &compositor->outputs, link)
    {
        pixman_region32_union_rect(&pointer_region, &pointer_region,
                                   output->geometry.x, output->geometry.y,
                                   output->geometry.width,
                                   output->geometry.height);
    }

    swc_seat_set_pointer_region(&compositor->seat, &pointer_region);
    pixman_region32_fini(&pointer_region);

    pixman_region32_init(&compositor->damage);
    pixman_region32_init(&compositor->opaque);
    wl_list_init(&compositor->surfaces);
    wl_array_init(&compositor->key_bindings);
    wl_signal_init(&compositor->destroy_signal);

    swc_compositor_add_key_binding(compositor,
        SWC_MOD_CTRL | SWC_MOD_ALT, XKB_KEY_BackSpace, &handle_terminate, display);

    for (keysym = XKB_KEY_XF86Switch_VT_1;
         keysym <= XKB_KEY_XF86Switch_VT_12;
         ++keysym)
    {
        swc_compositor_add_key_binding(compositor, SWC_MOD_ANY, keysym,
                                       &handle_switch_vt, &compositor->tty);
    }


    return true;

  error_renderer:
    swc_renderer_finalize(&compositor->renderer);
  error_drm:
    swc_drm_finish(&compositor->drm);
  error_seat:
    swc_seat_finish(&compositor->seat);
  error_tty:
    swc_tty_finish(&compositor->tty);
  error_udev:
    udev_unref(compositor->udev);
  error_base:
    return false;
}

void swc_compositor_finish(struct swc_compositor * compositor)
{
    struct swc_output * output, * tmp;

    wl_signal_emit(&compositor->destroy_signal, compositor);

    wl_array_release(&compositor->key_bindings);

    wl_list_for_each_safe(output, tmp, &compositor->outputs, link)
    {
        swc_output_finish(output);
        free(output);
    }

    swc_drm_finish(&compositor->drm);
    swc_seat_finish(&compositor->seat);
    swc_tty_finish(&compositor->tty);
    udev_unref(compositor->udev);
}

void swc_compositor_add_globals(struct swc_compositor * compositor,
                                struct wl_display * display)
{
    struct swc_output * output;

    wl_global_create(display, &wl_compositor_interface, 3, compositor,
                     &bind_compositor);

    swc_data_device_manager_add_globals(display);
    swc_seat_add_globals(&compositor->seat, display);
    swc_drm_add_globals(&compositor->drm, display);

    wl_list_for_each(output, &compositor->outputs, link)
    {
        swc_output_add_globals(output, display);
    }
}

void swc_compositor_add_key_binding(struct swc_compositor * compositor,
                                    uint32_t modifiers, uint32_t value,
                                    swc_binding_handler_t handler, void * data)
{
    struct swc_binding * binding;

    binding = wl_array_add(&compositor->key_bindings, sizeof *binding);
    binding->value = value;
    binding->modifiers = modifiers;
    binding->handler = handler;
    binding->data = data;
}

void swc_compositor_schedule_update(struct swc_compositor * compositor,
                                    struct swc_output * output)
{
    bool update_scheduled = compositor->scheduled_updates != 0;

    if (compositor->scheduled_updates & SWC_OUTPUT_MASK(output))
        return;

    compositor->scheduled_updates |= SWC_OUTPUT_MASK(output);

    if (!update_scheduled)
    {
        struct wl_event_loop * event_loop;

        event_loop = wl_display_get_event_loop(compositor->display);
        wl_event_loop_add_idle(event_loop, &perform_update, compositor);
    }
}

