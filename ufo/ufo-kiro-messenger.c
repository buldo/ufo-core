/*
 * Copyright (C) 2011-2015 Karlsruhe Institute of Technology
 *
 * This file is part of Ufo.
 *
 * This library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ufo/ufo-kiro-messenger.h>
#include <kiro/kiro-messenger.h>

static void ufo_messenger_interface_init (UfoMessengerIface *iface);

#define UFO_KIRO_MESSENGER_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), UFO_TYPE_KIRO_MESSENGER, UfoKiroMessengerPrivate))

G_DEFINE_TYPE_WITH_CODE (UfoKiroMessenger, ufo_kiro_messenger, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (UFO_TYPE_MESSENGER,
                                                ufo_messenger_interface_init))


struct _UfoKiroMessengerPrivate {
    KiroMessenger *km;
    gchar *remote_addr;
    gboolean ready;
    UfoMessengerRole role;
};

UfoKiroMessenger *
ufo_kiro_messenger_new (void)
{
    UfoKiroMessenger *msger;
    msger = UFO_KIRO_MESSENGER (g_object_new (UFO_TYPE_KIRO_MESSENGER, NULL));
    return msger;
}

static gboolean
kiro_listen_address_decode (gchar *addr_in, gchar **addr, gchar **port, GError **error)
{
    if (!g_str_has_prefix (addr_in, "kiro://")) {
        g_set_error_literal (error, UFO_MESSENGER_ERROR, UFO_MESSENGER_CONNECTION_PROBLEM,
                             "Address does not use 'kiro://' scheme.");
        return FALSE;
    }

    /* Pitfall: kiro will silently accept hostnames like kiro://localhost:5555
     * but not bind to it as it treats it like an interface name (like eth0).
     * We have to use IP addresses instead of DNS names.
     */
    gchar *host = g_strdup (&addr_in[7]);

    if (!g_ascii_isdigit (host[0]) && host[0] != '*')
        g_debug ("Treating address %s as interface device name. Use IP address if supplying a host was intended.", host);

    gchar **split = g_strsplit (host, ":", 2);

    if (!g_ascii_isdigit (*split[1])) {
            g_set_error (error, UFO_MESSENGER_ERROR, UFO_MESSENGER_CONNECTION_PROBLEM,
                         "Address '%s' has wrong format", addr_in);
            g_strfreev (split);
            g_free (host);
            return FALSE;
    }

    *addr = g_strdup (split[0]);
    *port = g_strdup (split[1]);

    g_strfreev (split);
    g_free (host);
    return TRUE;
}

static void
ufo_kiro_messenger_connect (UfoMessenger *msger,
                           const gchar *addr_in,
                           UfoMessengerRole role,
                           GError **error)
{
    UfoKiroMessengerPrivate *priv = UFO_KIRO_MESSENGER_GET_PRIVATE (msger);

    priv->remote_addr = g_strdup (addr_in);
    priv->role = role;

    enum KiroMessengerType kmr = KIRO_MESSENGER_SERVER;
    if (role == UFO_MESSENGER_CLIENT)
        kmr = KIRO_MESSENGER_CLIENT;

    gchar *addr;
    gchar *port;

    if (!kiro_listen_address_decode (priv->remote_addr, &addr, &port, error)) {
        g_free (priv->remote_addr);
        return;
    }

    gint ret = kiro_messenger_start (priv->km, addr, port, kmr);
    if (0 > ret) {
        g_set_error (error, UFO_MESSENGER_ERROR, UFO_MESSENGER_CONNECTION_PROBLEM,
                     "Failed to connect to '%s'", priv->remote_addr);
        g_free (priv->remote_addr);
    }
    else
        priv->ready = TRUE;

    g_free (addr);
    g_free (port);
    return;
}

static void
ufo_kiro_messenger_disconnect (UfoMessenger *msger)
{
    UfoKiroMessengerPrivate *priv = UFO_KIRO_MESSENGER_GET_PRIVATE (msger);

    kiro_messenger_stop (priv->km);
    priv->ready = FALSE;
    return;
}


static gboolean
ufo_kiro_messenger_send_callback (struct KiroMessage *msg, gpointer user_data)
{
    msg->message_handled = TRUE;
    gboolean *flag = (gboolean *)user_data;
    *flag = TRUE;
    return FALSE; // automatically deregister
}


static gboolean
ufo_kiro_messenger_recv_callback (struct KiroMessage *msg, gpointer user_data)
{
    UfoMessage *msg_out = g_malloc (sizeof (UfoMessage));
    msg_out->type = msg->msg;
    msg_out->data_size = msg->size;
    msg_out->data = msg->payload;

    msg->payload = NULL;
    msg->message_handled = TRUE;

    UfoMessage **ret = (UfoMessage **)user_data;
    *ret = msg_out;

    return FALSE; // automatically deregister
}


/**
 * ufo_kiro_messenger_send_blocking: (skip)
 * @msger: #UfoMessenger
 * @request_msg: Request message
 * @error: Location for an error or %NULL
 *
 * Send message in blocking way.
 */
static UfoMessage *
ufo_kiro_messenger_send_blocking (UfoMessenger *msger,
                                 UfoMessage *request_msg,
                                 GError **error)
{
    UfoKiroMessengerPrivate *priv = UFO_KIRO_MESSENGER_GET_PRIVATE (msger);

    if (!priv->ready) {
        g_set_error (error, UFO_MESSENGER_ERROR, UFO_MESSENGER_ERROR,
                     "Messenger is not connected");
        return NULL;
    }

    if (request_msg->type == UFO_MESSAGE_ACK && priv->role == UFO_MESSENGER_CLIENT)
        g_critical ("Clients can't send ACK messages");

    gboolean send_done = FALSE;
    kiro_messenger_add_send_callback (priv->km, ufo_kiro_messenger_send_callback, &send_done);

    UfoMessage *response = NULL;
    gulong recv_id = 0;
    if (request_msg->type != UFO_MESSAGE_ACK) {
        // We will get a response. Register a callback for it
        recv_id = kiro_messenger_add_receive_callback (priv->km, ufo_kiro_messenger_recv_callback, &response);
    }

    struct KiroMessage msg;
    msg.msg = request_msg->type;
    msg.size = request_msg->data_size;
    msg.payload = request_msg->data;

    kiro_messenger_submit_message (priv->km, &msg, FALSE);
    while (!send_done) {};

    if (msg.status != KIRO_MESSAGE_SEND_SUCCESS) {
        g_set_error (error, UFO_MESSENGER_ERROR, UFO_MESSENGER_ERROR,
                     "Failed to send message to '%s'", priv->remote_addr);
        kiro_messenger_remove_receive_callback (priv->km, recv_id);
        if (response)
            g_free (response);
        return NULL;
    }

    if (request_msg->type != UFO_MESSAGE_ACK) {
        //receive callback will automatically fill in the 'response' message for us
        while (!response) {};
    }

    return response;
}

/**
 * ufo_kiro_messenger_recv_blocking: (skip)
 * @msger: A #UfoMessenger
 * @error: Location for an error or %NULL
 *
 * Receive message in blocking way.
 */
static UfoMessage *
ufo_kiro_messenger_recv_blocking (UfoMessenger *msger,
                                 GError **error)
{
    UfoKiroMessengerPrivate *priv = UFO_KIRO_MESSENGER_GET_PRIVATE (msger);

    if (!priv->ready) {
        g_set_error (error, UFO_MESSENGER_ERROR, UFO_MESSENGER_ERROR,
                     "Messenger is not connected");
        return NULL;
    }

    UfoMessage *result = NULL;
    kiro_messenger_add_receive_callback (priv->km, ufo_kiro_messenger_recv_callback, &result);

    //receive callback will automatically fill in the 'result' message for us
    while (!result) {};

    return result;
}

static void
ufo_messenger_interface_init (UfoMessengerIface *iface)
{
    iface->connect = ufo_kiro_messenger_connect;
    iface->disconnect = ufo_kiro_messenger_disconnect;
    iface->send_blocking = ufo_kiro_messenger_send_blocking;
    iface->recv_blocking = ufo_kiro_messenger_recv_blocking;
}

static void
ufo_kiro_messenger_dispose (GObject *object)
{
    UfoKiroMessengerPrivate *priv = UFO_KIRO_MESSENGER_GET_PRIVATE (object);

    ufo_kiro_messenger_disconnect (UFO_MESSENGER (object));
    kiro_messenger_free (priv->km);
    priv->km = NULL;
}

static void
ufo_kiro_messenger_finalize (GObject *object)
{
    UfoKiroMessengerPrivate *priv = UFO_KIRO_MESSENGER_GET_PRIVATE (object);

    if  (priv->remote_addr != NULL) {
        g_free (priv->remote_addr);
        priv->remote_addr = NULL;
    }
}

static void
ufo_kiro_messenger_class_init (UfoKiroMessengerClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    oclass->dispose      = ufo_kiro_messenger_dispose;
    oclass->finalize     = ufo_kiro_messenger_finalize;

    g_type_class_add_private (klass, sizeof(UfoKiroMessengerPrivate));
}

static void
ufo_kiro_messenger_init (UfoKiroMessenger *msger)
{
    UfoKiroMessengerPrivate *priv = UFO_KIRO_MESSENGER_GET_PRIVATE (msger);
    priv->km = kiro_messenger_new ();
    priv->ready = FALSE;
}
