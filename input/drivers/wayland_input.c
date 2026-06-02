/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2015 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <pthread.h>

#include <fcntl.h>
#include <unistd.h>

#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <file/file_path.h>
#include <compat/strl.h>
#include <string/stdstring.h>
#include <retro_miscellaneous.h>

#include "../input_keymaps.h"

#include "../common/linux_common.h"

#include "../../gfx/common/wayland_common.h"

#include "../../command.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

/* TODO/FIXME -
 * fix game focus toggle */

/* Forward declaration */

void flush_wayland_fd(void *data);

#if defined(__linux__)
typedef struct wl_gpio_thread_state
{
   input_ctx_wayland_data_t *wl;
   pthread_t thread;
   bool running;
   bool started;
} wl_gpio_thread_state_t;

static wl_gpio_thread_state_t g_wl_gpio_thread;

static bool wl_evdev_test_bit(const unsigned long *bitset, unsigned bit)
{
   unsigned width = (unsigned)(sizeof(unsigned long) * 8);
   return bitset[bit / width] & (1UL << (bit % width));
}

static bool wl_evdev_has_front_panel_keys(int fd)
{
   unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long)) /
      (8 * sizeof(unsigned long))];
   unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
      (8 * sizeof(unsigned long))];

   memset(ev_bits, 0, sizeof(ev_bits));
   memset(key_bits, 0, sizeof(key_bits));

   if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
      return false;

   if (!wl_evdev_test_bit(ev_bits, EV_KEY))
      return false;

   if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
      return false;

#ifdef KEY_EJECTCD
   if (wl_evdev_test_bit(key_bits, KEY_EJECTCD))
      return true;
#endif
#ifdef KEY_PLAYPAUSE
   if (wl_evdev_test_bit(key_bits, KEY_PLAYPAUSE))
      return true;
#endif
#ifdef KEY_RESTART
   if (wl_evdev_test_bit(key_bits, KEY_RESTART))
      return true;
#endif
#ifdef KEY_POWER
   if (wl_evdev_test_bit(key_bits, KEY_POWER))
      return true;
#endif
#ifdef KEY_SLEEP
   if (wl_evdev_test_bit(key_bits, KEY_SLEEP))
      return true;
#endif

   return false;
}

static bool wl_find_gpio_keys_event(char *out_path, size_t len)
{
   FILE *file;
   bool in_gpio = false;
   char line[512];

   if (!out_path || !len)
      return false;

   out_path[0] = '\0';

   file = fopen("/proc/bus/input/devices", "rb");
   if (!file)
      return false;

   while (fgets(line, sizeof(line), file))
   {
      if (line[0] == 'N' && strstr(line, "Name=\"gpio-keys\""))
      {
         in_gpio = true;
         continue;
      }

      if (!in_gpio)
         continue;

      if (line[0] == 'H' && strstr(line, "Handlers="))
      {
         char *event_ptr = strstr(line, "event");

         while (event_ptr)
         {
            char *num_ptr = event_ptr + 5;
            int value     = 0;

            while (*num_ptr >= '0' && *num_ptr <= '9')
            {
               value = (value * 10) + (*num_ptr - '0');
               num_ptr++;
            }

            if (num_ptr > event_ptr + 5)
            {
               snprintf(out_path, len, "/dev/input/event%d", value);
               fclose(file);
               return true;
            }

            event_ptr = strstr(event_ptr + 5, "event");
         }
      }

      if (line[0] == '\n' || line[0] == '\r')
         in_gpio = false;
   }

   fclose(file);

   {
      int i;
      char path[64];

      for (i = 0; i < 64; i++)
      {
         int fd;

         snprintf(path, sizeof(path), "/dev/input/event%d", i);
         fd = open(path, O_RDONLY | O_NONBLOCK);
         if (fd < 0)
            continue;

         if (wl_evdev_has_front_panel_keys(fd))
         {
            close(fd);
            snprintf(out_path, len, "%s", path);
            return true;
         }

         close(fd);
      }
   }

   return false;
}

static void wl_close_gpio_keys(input_ctx_wayland_data_t *wl)
{
   if (!wl || wl->gpio_keys_fd < 0)
      return;

   {
      int grab = 0;
      ioctl(wl->gpio_keys_fd, EVIOCGRAB, grab);
   }

   close(wl->gpio_keys_fd);
   wl->gpio_keys_fd = -1;
}

static bool wl_open_gpio_keys(input_ctx_wayland_data_t *wl)
{
   char path[64];
   int fd;

   if (!wl)
      return false;

   if (wl->gpio_keys_fd >= 0)
      return true;

   if (!wl_find_gpio_keys_event(path, sizeof(path)))
   {
      if (!wl->gpio_keys_warned)
      {
         RARCH_WARN("[Wayland]: Could not locate gpio-keys in /proc/bus/input/devices.\n");
         wl->gpio_keys_warned = true;
      }
      return false;
   }

   fd = open(path, O_RDONLY | O_NONBLOCK);
   if (fd < 0)
   {
      RARCH_WARN("[Wayland]: Failed to open gpio-keys %s (%s).\n",
            path, strerror(errno));
      return false;
   }

   {
      int grab = 1;

      if (ioctl(fd, EVIOCGRAB, grab) != 0)
         RARCH_WARN("[Wayland]: EVIOCGRAB failed for %s (%s).\n",
               path, strerror(errno));
      else
         RARCH_LOG("[Wayland]: Grabbed gpio-keys device %s.\n", path);
   }

   wl->gpio_keys_fd = fd;
   wl->gpio_keys_warned = false;
   RARCH_LOG("[Wayland]: Opened gpio-keys device %s.\n", path);
   return true;
}

static void wl_poll_gpio_keys(input_ctx_wayland_data_t *wl)
{
   struct input_event ev;

   if (!wl)
      return;

   if (wl->gpio_keys_fd < 0)
   {
      wl_open_gpio_keys(wl);
      return;
   }

   for (;;)
   {
      ssize_t ret = read(wl->gpio_keys_fd, &ev, sizeof(ev));

      if (ret == (ssize_t)sizeof(ev))
      {
         if (ev.type != EV_KEY)
            continue;

#ifdef KEY_EJECTCD
         if (ev.code == KEY_EJECTCD && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_EJECTCD -> queue menu toggle.\n");
            wl->gpio_menu_toggle_pending = true;
            continue;
         }
#endif

#ifdef KEY_PLAYPAUSE
         if (ev.code == KEY_PLAYPAUSE && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_PLAYPAUSE -> queue quit.\n");
            wl->gpio_quit_pending = true;
            continue;
         }
#endif

#ifdef KEY_RESTART
         if (ev.code == KEY_RESTART && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_RESTART -> queue quit.\n");
            wl->gpio_quit_pending = true;
            continue;
         }
#endif

#ifdef KEY_POWER
         if (ev.code == KEY_POWER && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_POWER -> queue quit.\n");
            wl->gpio_quit_pending = true;
            continue;
         }
#endif

#ifdef KEY_EXIT
         if (ev.code == KEY_EXIT && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_EXIT -> queue quit.\n");
            wl->gpio_quit_pending = true;
            continue;
         }
#endif

#ifdef KEY_ESC
         if (ev.code == KEY_ESC && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_ESC -> queue quit.\n");
            wl->gpio_quit_pending = true;
            continue;
         }
#endif

#ifdef KEY_MENU
         if (ev.code == KEY_MENU && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_MENU -> queue menu toggle.\n");
            wl->gpio_menu_toggle_pending = true;
            continue;
         }
#endif

#ifdef KEY_HOME
         if (ev.code == KEY_HOME && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_HOME -> queue menu toggle.\n");
            wl->gpio_menu_toggle_pending = true;
            continue;
         }
#endif

#ifdef KEY_HOMEPAGE
         if (ev.code == KEY_HOMEPAGE && ev.value == 1)
         {
            RARCH_LOG("[Wayland]: gpio-keys KEY_HOMEPAGE -> queue menu toggle.\n");
            wl->gpio_menu_toggle_pending = true;
            continue;
         }
#endif

         continue;
      }

      if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
         break;

      if (ret < 0 && errno == EINTR)
         continue;

      RARCH_WARN("[Wayland]: Lost gpio-keys input device, retrying.\n");
      wl_close_gpio_keys(wl);
      break;
   }
}

static void *wl_gpio_thread_main(void *arg)
{
   wl_gpio_thread_state_t *st = (wl_gpio_thread_state_t*)arg;

   while (st->running)
   {
      struct pollfd pfd;
      int ret;

      if (!st->wl)
         break;

      if (st->wl->gpio_keys_fd < 0)
      {
         wl_open_gpio_keys(st->wl);
         usleep(200000);
         continue;
      }

      pfd.fd      = st->wl->gpio_keys_fd;
      pfd.events  = POLLIN | POLLERR | POLLHUP;
      pfd.revents = 0;

      ret = poll(&pfd, 1, 250);
      if (!st->running)
         break;

      if (ret < 0)
      {
         if (errno == EINTR)
            continue;
         wl_close_gpio_keys(st->wl);
         continue;
      }

      if (ret == 0)
         continue;

      if (pfd.revents & (POLLERR | POLLHUP))
      {
         wl_close_gpio_keys(st->wl);
         continue;
      }

      if (pfd.revents & POLLIN)
         wl_poll_gpio_keys(st->wl);
   }

   return NULL;
}

static void wl_start_gpio_thread(input_ctx_wayland_data_t *wl)
{
   if (!wl || g_wl_gpio_thread.started)
      return;

   memset(&g_wl_gpio_thread, 0, sizeof(g_wl_gpio_thread));
   g_wl_gpio_thread.wl      = wl;
   g_wl_gpio_thread.running = true;

   if (pthread_create(&g_wl_gpio_thread.thread, NULL, wl_gpio_thread_main, &g_wl_gpio_thread) == 0)
      g_wl_gpio_thread.started = true;
   else
   {
      g_wl_gpio_thread.running = false;
      RARCH_WARN("[Wayland]: Failed to start gpio-keys thread.\n");
   }
}

static void wl_stop_gpio_thread(void)
{
   if (!g_wl_gpio_thread.started)
      return;

   g_wl_gpio_thread.running = false;
   pthread_join(g_wl_gpio_thread.thread, NULL);
   memset(&g_wl_gpio_thread, 0, sizeof(g_wl_gpio_thread));
}
#endif

static int16_t input_wl_mouse_state(input_ctx_wayland_data_t *wl, unsigned id, bool screen)
{
   switch (id)
   {
      case RETRO_DEVICE_ID_MOUSE_X:
         return screen ? wl->mouse.x : wl->mouse.delta_x;
      case RETRO_DEVICE_ID_MOUSE_Y:
         return screen ? wl->mouse.y : wl->mouse.delta_y;
      case RETRO_DEVICE_ID_MOUSE_LEFT:
         return wl->mouse.left;
      case RETRO_DEVICE_ID_MOUSE_RIGHT:
         return wl->mouse.right;
      case RETRO_DEVICE_ID_MOUSE_MIDDLE:
         return wl->mouse.middle;

      /* TODO: Rest of the mouse inputs. */
   }

   return 0;
}

static int16_t input_wl_lightgun_state(input_ctx_wayland_data_t *wl, unsigned id)
{
   switch (id)
   {
      case RETRO_DEVICE_ID_LIGHTGUN_X:
         return wl->mouse.delta_x;
      case RETRO_DEVICE_ID_LIGHTGUN_Y:
         return wl->mouse.delta_y;
      case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
         return wl->mouse.left;
      case RETRO_DEVICE_ID_LIGHTGUN_CURSOR:
         return wl->mouse.middle;
      case RETRO_DEVICE_ID_LIGHTGUN_TURBO:
         return wl->mouse.right;
      case RETRO_DEVICE_ID_LIGHTGUN_START:
         return wl->mouse.middle && wl->mouse.right;
      case RETRO_DEVICE_ID_LIGHTGUN_PAUSE:
         return wl->mouse.middle && wl->mouse.left;
   }

   return 0;
}

/* forward declaration */
bool wayland_context_gettouchpos(void *data, unsigned id,
      unsigned* touch_x, unsigned* touch_y);

static void input_wl_touch_pool(void *data)
{
   int id;
   unsigned touch_x             = 0;
   unsigned touch_y             = 0;
   input_ctx_wayland_data_t *wl = (input_ctx_wayland_data_t*)data;

   if (!wl)
      return;

   for (id = 0; id < MAX_TOUCHES; id++)
   {
      if (wayland_context_gettouchpos(wl, id, &touch_x, &touch_y))
         wl->touches[id].active = true;
      else
         wl->touches[id].active = false;
      wl->touches[id].x         = touch_x;
      wl->touches[id].y         = touch_y;
   }
}

static void input_wl_poll(void *data)
{
   input_ctx_wayland_data_t *wl = (input_ctx_wayland_data_t*)data;
   if (!wl)
      return;

   flush_wayland_fd(wl);

   wl->mouse.delta_x = wl->mouse.x - wl->mouse.last_x;
   wl->mouse.delta_y = wl->mouse.y - wl->mouse.last_y;
   wl->mouse.last_x  = wl->mouse.x;
   wl->mouse.last_y  = wl->mouse.y;

   if (!wl->mouse.focus)
   {
      wl->mouse.delta_x = 0;
      wl->mouse.delta_y = 0;
   }

   if (wl->joypad)
      wl->joypad->poll();

   if (wl->gpio_menu_toggle_pending)
   {
      wl->gpio_menu_toggle_pending = false;
      RARCH_LOG("[Wayland]: gpio-keys dispatch menu toggle.\n");
      command_event(CMD_EVENT_MENU_TOGGLE, NULL);
   }

   if (wl->gpio_quit_pending)
   {
      wl->gpio_quit_pending = false;
      RARCH_LOG("[Wayland]: gpio-keys dispatch quit.\n");
      command_event(CMD_EVENT_QUIT, NULL);
   }

   input_wl_touch_pool(wl);
}

static int16_t input_wl_analog_pressed(input_ctx_wayland_data_t *wl,
      const struct retro_keybind *binds,
      unsigned idx, unsigned id)
{
   unsigned id_minus     = 0;
   unsigned id_plus      = 0;
   int16_t pressed_minus = 0;
   int16_t pressed_plus  = 0;

   input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

   if (binds
         && binds[id_minus].valid
         && (id_minus < RARCH_BIND_LIST_END)
         && BIT_GET(wl->key_state, rarch_keysym_lut[binds[id_minus].key])
      )
      pressed_minus = -0x7fff;
   if (binds
         && binds[id_plus].valid
         && (id_plus < RARCH_BIND_LIST_END)
         && BIT_GET(wl->key_state, rarch_keysym_lut[binds[id_plus].key])
      )
      pressed_plus = 0x7fff;

   return pressed_plus + pressed_minus;
}

static bool input_wl_state_kb(input_ctx_wayland_data_t *wl,
      const struct retro_keybind **binds,
      unsigned port, unsigned device, unsigned idx, unsigned id)
{
   unsigned bit = rarch_keysym_lut[(enum retro_key)id];
   return id < RETROK_LAST && BIT_GET(wl->key_state, bit);
}

static int16_t input_wl_pointer_state(input_ctx_wayland_data_t *wl,
      unsigned idx, unsigned id, bool screen)
{
   struct video_viewport vp;

   bool inside                 = false;
   int16_t res_x               = 0;
   int16_t res_y               = 0;
   int16_t res_screen_x        = 0;
   int16_t res_screen_y        = 0;

   vp.x                        = 0;
   vp.y                        = 0;
   vp.width                    = 0;
   vp.height                   = 0;
   vp.full_width               = 0;
   vp.full_height              = 0;

   if (!(video_driver_translate_coord_viewport_wrap(&vp,
         wl->mouse.x, wl->mouse.y,
         &res_x, &res_y, &res_screen_x, &res_screen_y)))

      return 0;

   if (screen)
   {
      res_x = res_screen_x;
      res_y = res_screen_y;
   }

   inside = (res_x >= -0x7fff) && (res_y >= -0x7fff);

   if (!inside)
      return 0;

   switch (id)
   {
      case RETRO_DEVICE_ID_POINTER_X:
         return res_x;
      case RETRO_DEVICE_ID_POINTER_Y:
         return res_y;
      case RETRO_DEVICE_ID_POINTER_PRESSED:
         return wl->mouse.left;
   }

   return 0;
}

static int16_t input_wl_touch_state(input_ctx_wayland_data_t *wl,
      unsigned idx, unsigned id, bool screen)
{
   struct video_viewport vp;

   bool inside                 = false;
   int16_t res_x               = 0;
   int16_t res_y               = 0;
   int16_t res_screen_x        = 0;
   int16_t res_screen_y        = 0;

   vp.x                        = 0;
   vp.y                        = 0;
   vp.width                    = 0;
   vp.height                   = 0;
   vp.full_width               = 0;
   vp.full_height              = 0;

   if (idx > MAX_TOUCHES)
      return 0;

   if (!(video_driver_translate_coord_viewport_wrap(&vp,
         wl->touches[idx].x, wl->touches[idx].y,
         &res_x, &res_y, &res_screen_x, &res_screen_y)))
      return 0;

   if (screen)
   {
      res_x = res_screen_x;
      res_y = res_screen_y;
   }

   inside = (res_x >= -0x7fff) && (res_y >= -0x7fff);

   if (!inside)
      return 0;

   switch (id)
   {
      case RETRO_DEVICE_ID_POINTER_X:
         return res_x;
      case RETRO_DEVICE_ID_POINTER_Y:
         return res_y;
      case RETRO_DEVICE_ID_POINTER_PRESSED:
         return wl->touches[idx].active;
   }

   return 0;
}

static int16_t input_wl_state(void *data,
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind **binds,
      unsigned port, unsigned device, unsigned idx, unsigned id)
{
   input_ctx_wayland_data_t *wl = (input_ctx_wayland_data_t*)data;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;
            int16_t ret = 0;
            for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
            {
               /* Auto-binds are per joypad, not per user. */
               const uint64_t joykey  = (binds[port][i].joykey != NO_BTN)
                  ? binds[port][i].joykey : joypad_info->auto_binds[i].joykey;
               const uint32_t joyaxis = (binds[port][i].joyaxis != AXIS_NONE)
                  ? binds[port][i].joyaxis : joypad_info->auto_binds[i].joyaxis;
               if (BIT_GET(wl->key_state, rarch_keysym_lut[binds[port][i].key]) )
               {
                  ret |= (1 << i);
                  continue;
               }

               if (binds[port])
               {
                  if ((uint16_t)joykey != NO_BTN && wl->joypad->button(joypad_info->joy_idx, (uint16_t)joykey))
                  {
                     ret |= (1 << i);
                     continue;
                  }
                  if (((float)abs(wl->joypad->axis(joypad_info->joy_idx, joyaxis)) / 0x8000) > joypad_info->axis_threshold)
                  {
                     ret |= (1 << i);
                     continue;
                  }
               }
            }

            return ret;
         }
         else
         {
            /* Auto-binds are per joypad, not per user. */
            const uint64_t joykey  = (binds[port][id].joykey != NO_BTN)
               ? binds[port][id].joykey : joypad_info->auto_binds[id].joykey;
            const uint32_t joyaxis = (binds[port][id].joyaxis != AXIS_NONE)
               ? binds[port][id].joyaxis : joypad_info->auto_binds[id].joyaxis;

            if (id < RARCH_BIND_LIST_END)
               if (BIT_GET(wl->key_state, rarch_keysym_lut[binds[port][id].key]))
                  return true;

            if (binds[port])
            {
               if ((uint16_t)joykey != NO_BTN && wl->joypad->button(joypad_info->joy_idx, (uint16_t)joykey))
                  return true;
               if (((float)abs(wl->joypad->axis(joypad_info->joy_idx, joyaxis)) / 0x8000) > joypad_info->axis_threshold)
                  return true;
            }
         }
         break;
      case RETRO_DEVICE_ANALOG:
         {
            int16_t ret = input_wl_analog_pressed(wl, binds[port], idx, id);
            if (!ret && binds[port])
               ret = input_joypad_analog(wl->joypad, joypad_info, port, idx, id, binds[port]);
            return ret;
         }
      case RETRO_DEVICE_KEYBOARD:
         return input_wl_state_kb(wl, binds, port, device, idx, id);
      case RETRO_DEVICE_MOUSE:
         return input_wl_mouse_state(wl, id, false);
      case RARCH_DEVICE_MOUSE_SCREEN:
         return input_wl_mouse_state(wl, id, true);

      case RETRO_DEVICE_POINTER:
         if (idx == 0)
            return input_wl_pointer_state(wl, idx, id,
                  device == RARCH_DEVICE_POINTER_SCREEN);
         break;
      case RARCH_DEVICE_POINTER_SCREEN:
         if (idx < MAX_TOUCHES)
            return input_wl_touch_state(wl, idx, id,
                  device == RARCH_DEVICE_POINTER_SCREEN);
         break;
      case RETRO_DEVICE_LIGHTGUN:
         return input_wl_lightgun_state(wl, id);
   }

   return 0;
}

static void input_wl_free(void *data)
{
   input_ctx_wayland_data_t *wl = (input_ctx_wayland_data_t*)data;
   if (!wl)
      return;

#if defined(__linux__)
   wl_stop_gpio_thread();
   wl_close_gpio_keys(wl);
   wl->gpio_menu_toggle_pending = false;
   wl->gpio_quit_pending = false;
#endif

   if (wl->joypad)
      wl->joypad->destroy();
}

bool input_wl_init(void *data, const char *joypad_name)
{
   input_ctx_wayland_data_t *wl = (input_ctx_wayland_data_t*)data;

   if (!wl)
      return false;

#if defined(__linux__)
   wl->gpio_keys_fd = -1;
   wl->gpio_menu_toggle_pending = false;
   wl->gpio_quit_pending = false;
   wl_open_gpio_keys(wl);
   wl_start_gpio_thread(wl);
#endif

   wl->joypad = input_joypad_init_driver(joypad_name, wl);

   if (!wl->joypad)
      return false;

   input_keymaps_init_keyboard_lut(rarch_key_map_linux);
   return true;
}

static uint64_t input_wl_get_capabilities(void *data)
{
   (void)data;

   return
      (1 << RETRO_DEVICE_JOYPAD)   |
      (1 << RETRO_DEVICE_ANALOG)   |
      (1 << RETRO_DEVICE_KEYBOARD) |
      (1 << RETRO_DEVICE_MOUSE)    |
      (1 << RETRO_DEVICE_LIGHTGUN);
}

static void input_wl_grab_mouse(void *data, bool state)
{
   /* Dummy for now. Might be useful in the future. */
   (void)data;
   (void)state;
}

static bool input_wl_set_rumble(void *data, unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   input_ctx_wayland_data_t *wl = (input_ctx_wayland_data_t*)data;
   if (wl && wl->joypad)
      return input_joypad_set_rumble(wl->joypad, port, effect, strength);
   return false;
}

static const input_device_driver_t *input_wl_get_joypad_driver(void *data)
{
   input_ctx_wayland_data_t *wl = (input_ctx_wayland_data_t*)data;
   if (!wl)
      return NULL;
   return wl->joypad;
}

input_driver_t input_wayland = {
   NULL,
   input_wl_poll,
   input_wl_state,
   input_wl_free,
   NULL,
   NULL,
   input_wl_get_capabilities,
   "wayland",
   input_wl_grab_mouse,
   NULL,
   input_wl_set_rumble,
   input_wl_get_joypad_driver,
   NULL,
   false
};
