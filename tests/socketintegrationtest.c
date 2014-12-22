/* GStreamer
 *
 * Copyright (C) 2014 William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <unistd.h>
#include <sys/socket.h>

#include <gio/gio.h>
#include <gst/check/gstcheck.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gio/gunixfdmessage.h>
#include "../build/gstnetcontrolmessagemeta.h"

static gboolean
g_socketpair (GSocketFamily family, GSocketType type, GSocketProtocol protocol,
    GSocket * gsv[2], GError ** error);

typedef struct
{
  GstElement *sink;
  GstElement *src;

  GstPipeline *sink_pipeline;
  GstPipeline *src_pipeline;
  GstAppSrc *sink_src;
  GstAppSink *src_sink;
} SymmetryTest;

static void
symmetry_test_setup (SymmetryTest * st, GstElement * sink, GstElement * src)
{
  GstCaps *caps;
  st->sink = sink;
  g_object_set (sink, "sync", FALSE, NULL);
  st->src = src;

  st->sink_pipeline = GST_PIPELINE (gst_pipeline_new (NULL));
  st->src_pipeline = GST_PIPELINE (gst_pipeline_new (NULL));

  st->sink_src = GST_APP_SRC (gst_element_factory_make ("appsrc", NULL));
  fail_unless (st->sink_src != NULL);
  caps = gst_caps_from_string ("application/x-gst-check");
  gst_app_src_set_caps (st->sink_src, caps);
  gst_caps_unref (caps);

  gst_bin_add_many (GST_BIN (st->sink_pipeline), GST_ELEMENT (st->sink_src),
      st->sink, NULL);
  fail_unless (gst_element_link_many (GST_ELEMENT (st->sink_src), st->sink,
          NULL));

  st->src_sink = GST_APP_SINK (gst_element_factory_make ("appsink", NULL));
  fail_unless (st->src_sink != NULL);
  gst_bin_add_many (GST_BIN (st->src_pipeline), st->src,
      GST_ELEMENT (st->src_sink), NULL);
  fail_unless (gst_element_link_many (st->src, GST_ELEMENT (st->src_sink),
          NULL));

  gst_element_set_state (GST_ELEMENT (st->sink_pipeline), GST_STATE_PLAYING);
  gst_element_set_state (GST_ELEMENT (st->src_pipeline), GST_STATE_PLAYING);
}

static void
symmetry_test_teardown (SymmetryTest * st)
{
  fail_unless (gst_element_set_state (GST_ELEMENT (st->sink_pipeline),
          GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE);
  fail_unless (gst_element_set_state (GST_ELEMENT (st->src_pipeline),
          GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE);

  gst_object_unref (st->sink_pipeline);
  gst_object_unref (st->src_pipeline);

  memset (st, 0, sizeof (*st));
}

static void
symmetry_test_assert_passthrough (SymmetryTest * st, GstBuffer * in)
{
  gpointer copy;
  gsize data_size;
  GstSample *out;

  gst_buffer_extract_dup (in, 0, -1, &copy, &data_size);

  fail_unless (gst_app_src_push_buffer (st->sink_src, in) == GST_FLOW_OK);
  in = NULL;
  out = gst_app_sink_pull_sample (st->src_sink);
  fail_unless (out != NULL);

  fail_unless (gst_buffer_get_size (gst_sample_get_buffer (out)) == data_size);
  fail_unless (gst_buffer_memcmp (gst_sample_get_buffer (out), 0, copy,
          data_size) == 0);
  g_free (copy);
  gst_sample_unref (out);
}

static gboolean
g_socketpair (GSocketFamily family, GSocketType type, GSocketProtocol protocol,
    GSocket * gsv[2], GError ** error)
{
  int ret;
  int sv[2];

  ret = socketpair (family, type, protocol, sv);
  if (ret != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "socketpair failed: %s",
        g_strerror (errno));
    return FALSE;
  }

  gsv[0] = g_socket_new_from_fd (sv[0], error);
  if (gsv[0] == NULL) {
    close (sv[0]);
    close (sv[1]);
    return FALSE;
  }
  gsv[1] = g_socket_new_from_fd (sv[1], error);
  if (gsv[1] == NULL) {
    g_object_unref (gsv[0]);
    gsv[0] = NULL;
    close (sv[1]);
    return FALSE;
  }
  return TRUE;
}

static void
setup_multisocketsink_and_socketsrc (SymmetryTest * st)
{
  GSocket *sockets[2] = { NULL, NULL };
  GError *err = NULL;

  st->sink = gst_check_setup_element ("pvmultisocketsink");
  st->src = gst_check_setup_element ("pvsocketsrc");

  fail_unless (g_socketpair (G_SOCKET_FAMILY_UNIX,
          G_SOCKET_TYPE_STREAM | SOCK_CLOEXEC, G_SOCKET_PROTOCOL_DEFAULT,
          sockets, &err));

  g_object_set (st->src, "socket", sockets[0], NULL);
  g_object_unref (sockets[0]);
  sockets[0] = NULL;

  symmetry_test_setup (st, st->sink, st->src);

  g_signal_emit_by_name (st->sink, "add", sockets[1], NULL);
  g_object_unref (sockets[1]);
  sockets[1] = NULL;
}

GST_START_TEST (test_that_socketsrc_and_multisocketsink_are_symmetrical)
{
  SymmetryTest st = { 0 };
  setup_multisocketsink_and_socketsrc (&st);
  symmetry_test_assert_passthrough (&st,
      gst_buffer_new_wrapped (g_strdup ("hello"), 5));
  symmetry_test_teardown (&st);
}

GST_END_TEST;

static Suite *
socketintegrationtest_suite (void)
{
  Suite *s = suite_create ("socketintegrationtest");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain,
      test_that_socketsrc_and_multisocketsink_are_symmetrical);
  tcase_add_test (tc_chain,
      test_that_multisocketsink_and_socketsrc_preserve_meta);

  return s;
}

GST_CHECK_MAIN (socketintegrationtest);
