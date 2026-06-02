/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2014-2015 - Higor Euripedes
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
#include <stdlib.h>

#include <boolean.h>
#include <string/stdstring.h>
#include <libretro.h>

#if defined(__linux__)
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#endif

#include "SDL.h"

#include "../input_keymaps.h"

#include "../../command.h"
#include "../../retroarch.h"
#include "../../verbosity.h"
#include "../../tasks/tasks_internal.h"

/* TODO/FIXME -
 * fix game focus toggle */

typedef struct sdl_input
{
   const input_device_driver_t *joypad;

   int mouse_x, mouse_y;
   int mouse_abs_x, mouse_abs_y;
   int mouse_l, mouse_r, mouse_m, mouse_b4, mouse_b5, mouse_wu, mouse_wd, mouse_wl, mouse_wr;
#if defined(__linux__)
   int gpio_keys_fd;
   bool gpio_keys_warned;
#endif
} sdl_input_t;

#if defined(__linux__)
static bool sdl_evdev_test_bit(const unsigned long *bitset, unsigned bit)
{
   unsigned width = (unsigned)(sizeof(unsigned long) * 8);
   return bitset[bit / width] & (1UL << (bit % width));
}

static bool sdl_evdev_has_front_panel_keys(int fd)
{
   unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long)) /
      (8 * sizeof(unsigned long))];
   unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
      (8 * sizeof(unsigned long))];

   memset(ev_bits, 0, sizeof(ev_bits));
   memset(key_bits, 0, sizeof(key_bits));

   if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
      return false;

   if (!sdl_evdev_test_bit(ev_bits, EV_KEY))
      return false;

   if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
      return false;

#ifdef KEY_EJECTCD
   if (sdl_evdev_test_bit(key_bits, KEY_EJECTCD))
      return true;
#endif
#ifdef KEY_PLAYPAUSE
   if (sdl_evdev_test_bit(key_bits, KEY_PLAYPAUSE))
      return true;
#endif
#ifdef KEY_RESTART
   if (sdl_evdev_test_bit(key_bits, KEY_RESTART))
      return true;
#endif
#ifdef KEY_POWER
   if (sdl_evdev_test_bit(key_bits, KEY_POWER))
      return true;
#endif
#ifdef KEY_SLEEP
   if (sdl_evdev_test_bit(key_bits, KEY_SLEEP))
      return true;
#endif

   return false;
}

static bool sdl_find_gpio_keys_event(char *out_path, size_t len)
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

         if (sdl_evdev_has_front_panel_keys(fd))
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

static void sdl_close_gpio_keys(sdl_input_t *sdl)
{
   if (!sdl || sdl->gpio_keys_fd < 0)
      return;

   {
      int grab = 0;
      ioctl(sdl->gpio_keys_fd, EVIOCGRAB, grab);
   }

   close(sdl->gpio_keys_fd);
   sdl->gpio_keys_fd = -1;
}

static bool sdl_open_gpio_keys(sdl_input_t *sdl)
{
   char path[64];
   int fd;

   if (!sdl)
      return false;

   if (sdl->gpio_keys_fd >= 0)
      return true;

   if (!sdl_find_gpio_keys_event(path, sizeof(path)))
   {
      if (!sdl->gpio_keys_warned)
      {
         RARCH_WARN("[SDL2]: Could not locate gpio-keys in /proc/bus/input/devices.\n");
         sdl->gpio_keys_warned = true;
      }
      return false;
   }

   fd = open(path, O_RDONLY | O_NONBLOCK);
   if (fd < 0)
   {
      RARCH_WARN("[SDL2]: Failed to open gpio-keys %s (%s).\n",
            path, strerror(errno));
      return false;
   }

   {
      int grab = 1;

      if (ioctl(fd, EVIOCGRAB, grab) != 0)
         RARCH_WARN("[SDL2]: EVIOCGRAB failed for %s (%s).\n",
               path, strerror(errno));
      else
         RARCH_LOG("[SDL2]: Grabbed gpio-keys device %s.\n", path);
   }

   sdl->gpio_keys_fd = fd;
   sdl->gpio_keys_warned = false;
   RARCH_LOG("[SDL2]: Opened gpio-keys device %s.\n", path);
   return true;
}

static void sdl_poll_gpio_keys(sdl_input_t *sdl)
{
   struct input_event ev;

   if (!sdl)
      return;

   if (sdl->gpio_keys_fd < 0)
   {
      sdl_open_gpio_keys(sdl);
      return;
   }

   for (;;)
   {
      ssize_t ret = read(sdl->gpio_keys_fd, &ev, sizeof(ev));

      if (ret == (ssize_t)sizeof(ev))
      {
         if (ev.type != EV_KEY)
            continue;

         if (ev.value != 0)
            RARCH_LOG("[SDL2]: gpio-keys event code=%u value=%d.\n",
                  (unsigned)ev.code, ev.value);

#ifdef KEY_EJECTCD
         if (ev.code == KEY_EJECTCD && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_EJECTCD -> menu toggle.\n");
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            continue;
         }
#endif

#ifdef KEY_PLAYPAUSE
         if (ev.code == KEY_PLAYPAUSE && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_PLAYPAUSE -> quit.\n");
            command_event(CMD_EVENT_QUIT, NULL);
            continue;
         }
#endif

#ifdef KEY_RESTART
         if (ev.code == KEY_RESTART && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_RESTART -> quit.\n");
            command_event(CMD_EVENT_QUIT, NULL);
            continue;
         }
#endif

#ifdef KEY_POWER
         if (ev.code == KEY_POWER && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_POWER -> quit.\n");
            command_event(CMD_EVENT_QUIT, NULL);
            continue;
         }
#endif

#ifdef KEY_EXIT
         if (ev.code == KEY_EXIT && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_EXIT -> quit.\n");
            command_event(CMD_EVENT_QUIT, NULL);
            continue;
         }
#endif

#ifdef KEY_ESC
         if (ev.code == KEY_ESC && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_ESC -> quit.\n");
            command_event(CMD_EVENT_QUIT, NULL);
            continue;
         }
#endif

#ifdef KEY_MENU
         if (ev.code == KEY_MENU && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_MENU -> menu toggle.\n");
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            continue;
         }
#endif

#ifdef KEY_HOME
         if (ev.code == KEY_HOME && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_HOME -> menu toggle.\n");
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            continue;
         }
#endif

#ifdef KEY_HOMEPAGE
         if (ev.code == KEY_HOMEPAGE && ev.value == 1)
         {
            RARCH_LOG("[SDL2]: gpio-keys KEY_HOMEPAGE -> menu toggle.\n");
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            continue;
         }
#endif

#ifdef KEY_SLEEP
         if (ev.code == KEY_SLEEP)
            continue;
#endif

         continue;
      }

      if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
         break;

      if (ret < 0 && errno == EINTR)
         continue;

      RARCH_WARN("[SDL2]: Lost gpio-keys input device, retrying.\n");
      sdl_close_gpio_keys(sdl);
      break;
   }
}
#endif

static void *sdl_input_init(const char *joypad_driver)
{
   sdl_input_t     *sdl = (sdl_input_t*)calloc(1, sizeof(*sdl));
   if (!sdl)
      return NULL;

#if defined(__linux__)
   sdl->gpio_keys_fd = -1;
#endif

   input_keymaps_init_keyboard_lut(rarch_key_map_sdl);

   sdl->joypad = input_joypad_init_driver(joypad_driver, sdl);

#if defined(__linux__)
   sdl_open_gpio_keys(sdl);
#endif

   RARCH_LOG("[SDL]: Input driver initialized.\n");
   return sdl;
}

static bool sdl_key_pressed(int key)
{
   int num_keys;
#ifdef HAVE_SDL2
   const uint8_t *keymap = SDL_GetKeyboardState(&num_keys);
   unsigned sym          = SDL_GetScancodeFromKey(rarch_keysym_lut[(enum retro_key)key]);
#else
   const uint8_t *keymap = SDL_GetKeyState(&num_keys);
   unsigned sym          = rarch_keysym_lut[(enum retro_key)key];
#endif

   if (sym >= (unsigned)num_keys)
      return false;

   return keymap[sym];
}

static int16_t sdl_analog_pressed(sdl_input_t *sdl, const struct retro_keybind *binds,
      unsigned idx, unsigned id)
{
   int16_t pressed_minus = 0, pressed_plus = 0;
   unsigned id_minus = 0;
   unsigned id_plus  = 0;

   input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

   if ((binds[id_minus].key < RETROK_LAST) && sdl_key_pressed(binds[id_minus].key))
      pressed_minus = -0x7fff;
   if ((binds[id_plus].key  < RETROK_LAST) && sdl_key_pressed(binds[id_plus].key))
      pressed_plus  = 0x7fff;

   return pressed_plus + pressed_minus;
}

static int16_t sdl_joypad_device_state(sdl_input_t *sdl,
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind *binds,
      unsigned port, unsigned id, enum input_device_type *device)
{
   /* Auto-binds are per joypad, not per user. */
   const uint64_t joykey  = (binds[id].joykey != NO_BTN)
      ? binds[id].joykey : joypad_info->auto_binds[id].joykey;
   const uint32_t joyaxis = (binds[id].joyaxis != AXIS_NONE)
      ? binds[id].joyaxis : joypad_info->auto_binds[id].joyaxis;

   if ((binds[id].key < RETROK_LAST) && sdl_key_pressed(binds[id].key))
   {
      *device = INPUT_DEVICE_TYPE_KEYBOARD;
      return 1;
   }

   if ((uint16_t)joykey != NO_BTN && sdl->joypad->button(
            joypad_info->joy_idx, (uint16_t)joykey))
   {
      *device = INPUT_DEVICE_TYPE_JOYPAD;
      return 1;
   }

   if (((float)abs(sdl->joypad->axis(joypad_info->joy_idx, joyaxis)) / 0x8000) > joypad_info->axis_threshold)
   {
      *device = INPUT_DEVICE_TYPE_JOYPAD;
      return 1;
   }

   return 0;
}

static int16_t sdl_mouse_device_state(sdl_input_t *sdl, unsigned id)
{
   switch (id)
   {
      case RETRO_DEVICE_ID_MOUSE_LEFT:
         return sdl->mouse_l;
      case RETRO_DEVICE_ID_MOUSE_RIGHT:
         return sdl->mouse_r;
      case RETRO_DEVICE_ID_MOUSE_WHEELUP:
         return sdl->mouse_wu;
      case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
         return sdl->mouse_wd;
      case RETRO_DEVICE_ID_MOUSE_X:
         return sdl->mouse_x;
      case RETRO_DEVICE_ID_MOUSE_Y:
         return sdl->mouse_y;
      case RETRO_DEVICE_ID_MOUSE_MIDDLE:
         return sdl->mouse_m;
      case RETRO_DEVICE_ID_MOUSE_BUTTON_4:
         return sdl->mouse_b4;
      case RETRO_DEVICE_ID_MOUSE_BUTTON_5:
         return sdl->mouse_b5;
   }

   return 0;
}

static int16_t sdl_pointer_device_state(sdl_input_t *sdl,
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

   if (!(video_driver_translate_coord_viewport_wrap(&vp, sdl->mouse_abs_x, sdl->mouse_abs_y,
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
         return sdl->mouse_l;
   }

   return 0;
}

static int16_t sdl_lightgun_device_state(sdl_input_t *sdl, unsigned id)
{
   switch (id)
   {
      case RETRO_DEVICE_ID_LIGHTGUN_X:
         return sdl->mouse_x;
      case RETRO_DEVICE_ID_LIGHTGUN_Y:
         return sdl->mouse_y;
      case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
         return sdl->mouse_l;
      case RETRO_DEVICE_ID_LIGHTGUN_CURSOR:
         return sdl->mouse_m;
      case RETRO_DEVICE_ID_LIGHTGUN_TURBO:
         return sdl->mouse_r;
      case RETRO_DEVICE_ID_LIGHTGUN_START:
         return sdl->mouse_m && sdl->mouse_r;
      case RETRO_DEVICE_ID_LIGHTGUN_PAUSE:
         return sdl->mouse_m && sdl->mouse_l;
   }

   return 0;
}

static int16_t sdl_input_state(void *data,
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind **binds,
      unsigned port, unsigned device, unsigned idx, unsigned id)
{
   enum input_device_type type = INPUT_DEVICE_TYPE_NONE;
   sdl_input_t            *sdl = (sdl_input_t*)data;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;
            int16_t ret = 0;

            for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
            {
               if (sdl_joypad_device_state(
                        sdl, joypad_info, binds[port], port, i, &type))
               {
                  ret |= (1 << i);
                  continue;
               }
            }

            return ret;
         }
         else
         {
            if (id < RARCH_BIND_LIST_END)
               if (sdl_joypad_device_state(sdl,
                     joypad_info, binds[port], port, id, &type))
                  return true;
         }
         break;
      case RETRO_DEVICE_ANALOG:
         if (binds[port])
         {
            int16_t ret = sdl_analog_pressed(sdl, binds[port], idx, id);
            if (!ret)
               ret = input_joypad_analog(sdl->joypad,
                        joypad_info, port, idx, id, binds[port]);
            return ret;
         }
         break;
      case RETRO_DEVICE_MOUSE:
         return sdl_mouse_device_state(sdl, id);
      case RETRO_DEVICE_POINTER:
      case RARCH_DEVICE_POINTER_SCREEN:
         if (idx == 0)
            return sdl_pointer_device_state(sdl, idx, id,
                  device == RARCH_DEVICE_POINTER_SCREEN);
         break;
      case RETRO_DEVICE_KEYBOARD:
         return (id < RETROK_LAST) && sdl_key_pressed(id);
      case RETRO_DEVICE_LIGHTGUN:
         return sdl_lightgun_device_state(sdl, id);
   }

   return 0;
}

static void sdl_input_free(void *data)
{
#ifndef HAVE_SDL2
   SDL_Event event;
#endif
   sdl_input_t *sdl = (sdl_input_t*)data;

   if (!data)
      return;

   /* Flush out all pending events. */
#ifdef HAVE_SDL2
   SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
#else
   while (SDL_PollEvent(&event));
#endif

   if (sdl->joypad)
      sdl->joypad->destroy();

#if defined(__linux__)
   sdl_close_gpio_keys(sdl);
#endif

   free(data);
}

static void sdl_grab_mouse(void *data, bool state)
{
#ifdef HAVE_SDL2
   struct temp{
      SDL_Window *w;
   };

   if (string_is_not_equal(video_driver_get_ident(), "sdl2"))
      return;

   /* First member of sdl2_video_t is the window */
   SDL_SetWindowGrab(((struct temp*)video_driver_get_ptr(false))->w,
         state ? SDL_TRUE : SDL_FALSE);
#endif
}

static bool sdl_set_rumble(void *data, unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   sdl_input_t *sdl = (sdl_input_t*)data;
   if (!sdl)
      return false;
   return input_joypad_set_rumble(sdl->joypad, port, effect, strength);
}

static const input_device_driver_t *sdl_get_joypad_driver(void *data)
{
   sdl_input_t *sdl = (sdl_input_t*)data;
   if (!sdl)
      return NULL;
   return sdl->joypad;
}

static void sdl_poll_mouse(sdl_input_t *sdl)
{
   Uint8 btn = SDL_GetRelativeMouseState(&sdl->mouse_x, &sdl->mouse_y);

   SDL_GetMouseState(&sdl->mouse_abs_x, &sdl->mouse_abs_y);

   sdl->mouse_l  = (SDL_BUTTON(SDL_BUTTON_LEFT)      & btn) ? 1 : 0;
   sdl->mouse_r  = (SDL_BUTTON(SDL_BUTTON_RIGHT)     & btn) ? 1 : 0;
   sdl->mouse_m  = (SDL_BUTTON(SDL_BUTTON_MIDDLE)    & btn) ? 1 : 0;
   sdl->mouse_b4 = (SDL_BUTTON(SDL_BUTTON_X1)        & btn) ? 1 : 0;
   sdl->mouse_b5 = (SDL_BUTTON(SDL_BUTTON_X2)        & btn) ? 1 : 0;
#ifndef HAVE_SDL2
   sdl->mouse_wu = (SDL_BUTTON(SDL_BUTTON_WHEELUP)   & btn) ? 1 : 0;
   sdl->mouse_wd = (SDL_BUTTON(SDL_BUTTON_WHEELDOWN) & btn) ? 1 : 0;
#endif
}

static void sdl_input_poll(void *data)
{
   sdl_input_t *sdl = (sdl_input_t*)data;
   SDL_Event event;

   SDL_PumpEvents();

   if (sdl->joypad)
      sdl->joypad->poll();
   sdl_poll_mouse(sdl);

#if defined(__linux__)
   sdl_poll_gpio_keys(sdl);
#endif

#ifdef HAVE_SDL2
   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_KEYDOWN, SDL_MOUSEWHEEL) > 0)
#else
   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_KEYEVENTMASK) > 0)
#endif
   {
      if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
      {
         uint16_t mod = 0;
         unsigned code = input_keymaps_translate_keysym_to_rk(event.key.keysym.sym);

#ifdef HAVE_SDL2
         if (event.type == SDL_KEYDOWN && event.key.repeat == 0)
         {
            switch (event.key.keysym.scancode)
            {
#ifdef SDL_SCANCODE_SLEEP
               case SDL_SCANCODE_SLEEP:
                  RARCH_LOG("[SDL2]: Front panel SLEEP ignored.\n");
                  continue;
#endif
#ifdef SDL_SCANCODE_AUDIOPLAY
               case SDL_SCANCODE_AUDIOPLAY:
                  RARCH_LOG("[SDL2]: Front panel AUDIOPLAY -> quit.\n");
                  command_event(CMD_EVENT_QUIT, NULL);
                  continue;
#endif
#ifdef SDL_SCANCODE_EJECT
               case SDL_SCANCODE_EJECT:
                  RARCH_LOG("[SDL2]: Front panel EJECT -> menu toggle.\n");
                  command_event(CMD_EVENT_MENU_TOGGLE, NULL);
                  continue;
#endif
               default:
                  break;
            }
         }

         /* PSC front-panel buttons come through SDL2 as media/eject scancodes.
          * Map them to RETROK_MENU so existing PSC configs bound to "menu"
          * can consume both RESET and OPEN consistently. */
#ifdef SDL_SCANCODE_SLEEP
         if (event.key.keysym.scancode == SDL_SCANCODE_SLEEP)
            continue;
#endif
#ifdef SDL_SCANCODE_EJECT
         if (event.key.keysym.scancode == SDL_SCANCODE_EJECT)
            code = RETROK_MENU;
#endif
#ifdef SDL_SCANCODE_AUDIOPLAY
         if (event.key.keysym.scancode == SDL_SCANCODE_AUDIOPLAY)
            code = RETROK_MENU;
#endif
#endif

         if (event.key.keysym.mod & KMOD_SHIFT)
            mod |= RETROKMOD_SHIFT;

         if (event.key.keysym.mod & KMOD_CTRL)
            mod |= RETROKMOD_CTRL;

         if (event.key.keysym.mod & KMOD_ALT)
            mod |= RETROKMOD_ALT;

         if (event.key.keysym.mod & KMOD_NUM)
            mod |= RETROKMOD_NUMLOCK;

         if (event.key.keysym.mod & KMOD_CAPS)
            mod |= RETROKMOD_CAPSLOCK;

         input_keyboard_event(event.type == SDL_KEYDOWN, code, code, mod,
               RETRO_DEVICE_KEYBOARD);
      }
#ifdef HAVE_SDL2
      else if (event.type == SDL_MOUSEWHEEL)
      {
         sdl->mouse_wu = event.wheel.y < 0;
         sdl->mouse_wd = event.wheel.y > 0;
         sdl->mouse_wl = event.wheel.x < 0;
         sdl->mouse_wr = event.wheel.x > 0;
         break;
      }
#endif
   }
}

static uint64_t sdl_get_capabilities(void *data)
{
   uint64_t caps = 0;

   caps |= (1 << RETRO_DEVICE_JOYPAD);
   caps |= (1 << RETRO_DEVICE_MOUSE);
   caps |= (1 << RETRO_DEVICE_KEYBOARD);
   caps |= (1 << RETRO_DEVICE_LIGHTGUN);
   caps |= (1 << RETRO_DEVICE_POINTER);
   caps |= (1 << RETRO_DEVICE_ANALOG);

   return caps;
}

input_driver_t input_sdl = {
   sdl_input_init,
   sdl_input_poll,
   sdl_input_state,
   sdl_input_free,
   NULL,
   NULL,
   sdl_get_capabilities,
#ifdef HAVE_SDL2
   "sdl2",
#else
   "sdl",
#endif
   sdl_grab_mouse,
   NULL,
   sdl_set_rumble,
   sdl_get_joypad_driver,
   NULL,
   false
};
