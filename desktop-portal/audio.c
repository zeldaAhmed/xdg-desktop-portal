/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "audio.h"

#include <stdio.h>

#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>
#include <glib/gi18n.h>

#include "pipewire.h"
#include "xdp-context.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-permissions.h"
#include "xdp-portal-config.h"
#include "xdp-pw-keys.h"
#include "xdp-request.h"
#include "xdp-utils.h"
#include "xdp-types.h"
#include "xdp-request-dex.h"
#include "xdp-utils.h"

typedef struct _Audio Audio;
typedef struct _AudioClass AudioClass;

struct _Audio
{
  XdpDbusAudioSkeleton parent_instance;

  XdpDbusImplAudio *impl;
  XdpContext *context;
};

struct _AudioClass
{
  XdpDbusAudioSkeletonClass parent_class;
};

GType audio_get_type (void);
static void audio_iface_init (XdpDbusAudioIface *iface);

G_DEFINE_TYPE_WITH_CODE (Audio, audio, XDP_DBUS_TYPE_AUDIO_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_AUDIO,
                                                audio_iface_init));

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Audio, g_object_unref)

/*
 * Auxuilary Methods
 */

static PipeWireRemote *
open_pipewire_audio_remote (const char  *app_id,
                            GError     **error)

{
  PipeWireRemote *remote;
  struct pw_permission permission_items[3];
  struct pw_properties *pipewire_properties;

  remote = pipewire_remote_new_sync (NULL, NULL, NULL, NULL, NULL, error);

  if (!remote)
    return NULL;

  /*
   * For now, hide all existing and future nodes by default. PipeWire will
   * consult the permission store to set up permissions afterwards.
   */
  permission_items[0] = PW_PERMISSION_INIT (PW_ID_CORE, PW_PERM_RWX);
  permission_items[1] = PW_PERMISSION_INIT (remote->node_factory_id, PW_PERM_R);
  permission_items[2] = PW_PERMISSION_INIT (PW_ID_ANY, 0);

  pw_client_update_permissions (pw_core_get_client(remote->core),
                                G_N_ELEMENTS (permission_items),
                                permission_items);

  pipewire_remote_roundtrip (remote);

  return remote;
}

/*
 * Callbacks
 */

static DexFuture*
handle_input_fiber_func (XdpDbusAudio          *object,
                         GDBusMethodInvocation *invocation,
                         const char            *arg_parent_window,
                         GVariant              *arg_options)
{
  g_autoptr(XdpDbusImplAudioRequestInputDevicesResult) result = NULL;
  g_autoptr(XdpRequestDex) request = NULL;
  g_autoptr(GError) error = NULL;
  Audio *audio = (Audio *) object;
  XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);

  request = dex_await_object (xdp_request_dex_new (
                audio->context,
                app_info,
                G_DBUS_INTERFACE_SKELETON (object),
                G_DBUS_PROXY (audio->impl),
                arg_options),
              &error);

  if (!request)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return dex_future_new_false ();
    }

  if (!xdp_request_dex_export (request, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              error);
      return dex_future_new_false ();
    }

  xdp_dbus_audio_complete_request_input_devices (object,
                                                 g_steal_pointer (&invocation),
                                                 xdp_request_dex_get_object_path (request));

  result = dex_await_boxed (xdp_dbus_impl_audio_call_request_input_devices_future (
                audio->impl,
                xdp_request_dex_get_object_path (request),
                xdp_app_info_get_id (app_info),
                arg_parent_window,
                arg_options),
              &error);

  /* TODO - actually process result to decide what response to send */

  xdp_request_dex_emit_response (request,
                                 XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
                                 NULL);

  return dex_future_new_true ();
}

/*
 * DbusAudioIface overrides
 */

static gboolean
handle_request_output_devices (XdpDbusAudio          *object,
                               GDBusMethodInvocation *invocation,
                               GVariant              *arg_options)
{
  g_debug ("Handle Access Output Devices - TODO");
}

static gboolean
handle_request_input_devices (XdpDbusAudio          *object,
                              GDBusMethodInvocation *invocation,
                              const char            *arg_parent_window,
                              GVariant              *arg_options)
{
  g_debug ("Handle Access Input Devices :3");

  /* todo - Do I need to spawn fiber manually, am I doing this all wrong? */
  dex_scheduler_spawnv (NULL, 0,
                        G_CALLBACK (handle_input_fiber_func),
                        4,
                        XDP_DBUS_TYPE_AUDIO, object,
                        G_TYPE_DBUS_METHOD_INVOCATION, invocation,
                        G_TYPE_STRING, arg_parent_window,
                        G_TYPE_VARIANT, arg_options);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_open_pipewire_remote (XdpDbusAudio          *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList           *in_fd_list,
                             GVariant              *arg_options)
{
  Audio *audio = (Audio *) object;
  XdpAppInfo *app_info;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  int fd, fd_id = -1;
  g_autoptr(GError) error = NULL;
  PipeWireRemote *remote;

  g_debug ("Handle Pipewire Remote");

  app_info = xdp_invocation_get_app_info (invocation);

  remote = open_pipewire_audio_remote (xdp_app_info_get_id (app_info), &error);

  if (!remote)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to open PipeWire remote: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  out_fd_list = g_unix_fd_list_new ();
  fd = pw_core_steal_fd (remote->core);
  fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
  close (fd);
  pipewire_remote_destroy (remote);

  xdp_dbus_audio_complete_open_pipewire_remote (object, invocation,
                                                out_fd_list,
                                                g_variant_new_handle (fd_id));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
audio_iface_init (XdpDbusAudioIface *iface)
{
  iface->handle_request_input_devices = handle_request_input_devices;
  iface->handle_open_pipewire_remote = handle_open_pipewire_remote;
}

/*
 * GObject overrides
 */

static void
audio_dispose (GObject *object)
{
  Audio *audio = (Audio *) object;

  g_clear_object (&audio->impl);

  G_OBJECT_CLASS (audio_parent_class)->dispose (object);
}

static void
audio_init (Audio *audio)
{
}

static void
audio_class_init (AudioClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = audio_dispose;
}

static Audio *
audio_new (XdpContext       *context,
           XdpDbusImplAudio *impl)
{
  Audio *audio;

  audio = g_object_new (audio_get_type (), NULL);

  audio->context = context;
  audio->impl = g_object_ref (impl);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (audio->impl), G_MAXINT);
  xdp_dbus_audio_set_version (XDP_DBUS_AUDIO (audio), 1);

  return audio;
}

DexFuture *
init_audio (gpointer user_data)
{
  g_autoptr(Audio) audio = NULL;
  g_autoptr(XdpDbusImplAudio) impl = NULL;
  g_autoptr(GError) error = NULL;
  XdpContext *context;
  XdpImplConfig *impl_config;
  GDBusConnection *connection;
  XdpPortalConfig *config;
  XdpDbusImplAccess *access_impl;

  context = XDP_CONTEXT (user_data);
  connection = xdp_context_get_connection (context);
  config = xdp_context_get_config (context);

  impl_config = xdp_portal_config_find (config, AUDIO_DBUS_IMPL_IFACE);

  if (impl_config == NULL)
    {
      g_debug ("No impl config for audio");
      return dex_future_new_true ();
    }

  impl = dex_await_object (xdp_dbus_impl_audio_proxy_new_future (
      connection,
      G_DBUS_PROXY_FLAGS_NONE,
      impl_config->dbus_name,
      DESKTOP_DBUS_PATH),
    &error);

  if (impl == NULL || error)
    {
      g_warning ("Failed to create audio proxy: %s", error->message);
      return dex_future_new_false ();
    }

  audio = audio_new (context, impl);
  xdp_context_take_and_export_portal (context,
                                      G_DBUS_INTERFACE_SKELETON (g_steal_pointer (&audio)),
                                      XDP_CONTEXT_EXPORT_FLAGS_RUN_IN_THREAD);

  return dex_future_new_true ();
}

