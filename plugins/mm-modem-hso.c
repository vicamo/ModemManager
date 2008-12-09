/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <dbus/dbus-glib.h>
#include "mm-modem-hso.h"
#include "mm-serial.h"
#include "mm-serial-parsers.h"
#include "mm-errors.h"
#include "mm-util.h"
#include "mm-callback-info.h"

static void impl_hso_authenticate (MMModemHso *self,
                                   const char *username,
                                   const char *password,
                                   DBusGMethodInvocation *context);

static void impl_hso_get_ip4_config (MMModemHso *self,
                                     DBusGMethodInvocation *context);

#include "mm-modem-gsm-hso-glue.h"

static gpointer mm_modem_hso_parent_class = NULL;

#define MM_MODEM_HSO_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), MM_TYPE_MODEM_HSO, MMModemHsoPrivate))

typedef struct {
    char *network_device;
    GRegex *connection_enabled_regex;
    gpointer std_parser;

    /* Pending connection attempt */
    MMCallbackInfo *connect_pending_data;
    guint connect_pending_id;
} MMModemHsoPrivate;

enum {
    PROP_0,
    PROP_NETWORK_DEVICE,

    LAST_PROP
};

#define OWANDATA_TAG "_OWANDATA: "

MMModem *
mm_modem_hso_new (const char *serial_device,
                  const char *network_device,
                  const char *driver)
{
    g_return_val_if_fail (serial_device != NULL, NULL);
    g_return_val_if_fail (network_device != NULL, NULL);
    g_return_val_if_fail (driver != NULL, NULL);

    return MM_MODEM (g_object_new (MM_TYPE_MODEM_HSO,
                                   MM_SERIAL_DEVICE, serial_device,
                                   MM_SERIAL_SEND_DELAY, (guint64) 10000,
                                   MM_MODEM_DRIVER, driver,
                                   MM_MODEM_HSO_NETWORK_DEVICE, network_device,
                                   NULL));
}

static void
hso_enable_done (MMSerial *serial,
                 GString *response,
                 GError *error,
                 gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;

    if (error)
        info->error = g_error_copy (error);

    mm_callback_info_schedule (info);
}

static guint32
hso_get_cid (MMModemHso *self)
{
    guint32 cid;

    cid = mm_generic_gsm_get_cid (MM_GENERIC_GSM (self));
    if (cid == 0)
        cid = 1;

    return cid;
}

static void
hso_enable (MMModemHso *self,
            gboolean enabled,
            MMModemFn callback,
            gpointer user_data)
{
    MMCallbackInfo *info;
    char *command;

    info = mm_callback_info_new (MM_MODEM (self), callback, user_data);

    command = g_strdup_printf ("AT_OWANCALL=%d,%d,1", hso_get_cid (self), enabled ? 1 : 0);
    mm_serial_queue_command (MM_SERIAL (self), command, 3, hso_enable_done, info);
    g_free (command);
}

static void
connect_pending_done (MMModemHso *self)
{
    MMModemHsoPrivate *priv = MM_MODEM_HSO_GET_PRIVATE (self);

    if (priv->connect_pending_data) {
        mm_callback_info_schedule (priv->connect_pending_data);
        priv->connect_pending_data = NULL;
    }

    if (priv->connect_pending_id) {
        g_source_remove (priv->connect_pending_id);
        priv->connect_pending_id = 0;
    }
}

static gboolean
hso_connect_timed_out (gpointer data)
{
    MMModemHsoPrivate *priv = MM_MODEM_HSO_GET_PRIVATE (data);

    priv->connect_pending_data->error = g_error_new_literal (MM_SERIAL_ERROR,
                                                             MM_SERIAL_RESPONSE_TIMEOUT,
                                                             "Connection timed out");
    connect_pending_done (MM_MODEM_HSO (data));

    return FALSE;
}

static void
hso_enabled (MMModem *modem,
             GError *error,
             gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;

    if (error) {
        info->error = g_error_copy (error);
        mm_callback_info_schedule (info);
    } else {
        MMModemHsoPrivate *priv = MM_MODEM_HSO_GET_PRIVATE (modem);
        GSource *source;

        source = g_timeout_source_new_seconds (30);
        g_source_set_closure (source, g_cclosure_new_object (G_CALLBACK (hso_connect_timed_out), G_OBJECT (modem)));
        g_source_attach (source, NULL);
        priv->connect_pending_data = info;
        priv->connect_pending_id = g_source_get_id (source);
        g_source_unref (source);
    }
}

static void
hso_disabled (MMModem *modem,
              GError *error,
              gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;

    if (error) {
        info->error = g_error_copy (error);
        mm_callback_info_schedule (info);
    } else
        hso_enable (MM_MODEM_HSO (modem), TRUE, hso_enabled, info);
}

static void
auth_done (MMSerial *serial,
           GString *response,
           GError *error,
           gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;

    if (error) {
        info->error = g_error_copy (error);
        mm_callback_info_schedule (info);
    } else
        /* success, kill any existing connections first */
        hso_enable (MM_MODEM_HSO (serial), FALSE, hso_disabled, info);
}

void
mm_hso_modem_authenticate (MMModemHso *self,
                           const char *username,
                           const char *password,
                           MMModemFn callback,
                           gpointer user_data)
{
    MMCallbackInfo *info;

    g_return_if_fail (MM_IS_MODEM_HSO (self));
    g_return_if_fail (callback != NULL);

    info = mm_callback_info_new (MM_MODEM (self), callback, user_data);

    if (username || password) {
        char *command;

        command = g_strdup_printf ("AT$QCPDPP=%d,1,\"%s\",\"%s\"",
                                   hso_get_cid (self),
                                   password ? password : "",
                                   username ? username : "");

        mm_serial_queue_command (MM_SERIAL (self), command, 3, auth_done, info);
        g_free (command);
    } else
        auth_done (MM_SERIAL (self), NULL, NULL, info);
}

static void
free_dns_array (gpointer data)
{
    g_array_free ((GArray *) data, TRUE);
}

static void
ip4_config_invoke (MMCallbackInfo *info)
{
    MMModemHsoIp4Fn callback = (MMModemHsoIp4Fn) info->callback;

    callback (MM_MODEM_HSO (info->modem),
              GPOINTER_TO_UINT (mm_callback_info_get_data (info, "ip4-address")),
              (GArray *) mm_callback_info_get_data (info, "ip4-dns"),
              info->error, info->user_data);
}

static void
get_ip4_config_done (MMSerial *serial,
                     GString *response,
                     GError *error,
                     gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;
	char **items, **iter;
    GArray *dns_array;
    int i;
    guint32 tmp;
    guint cid;

    if (error) {
        info->error = g_error_copy (error);
        goto out;
    } else if (!g_str_has_prefix (response->str, OWANDATA_TAG)) {
        info->error = g_error_new_literal (MM_MODEM_ERROR, MM_MODEM_ERROR_GENERAL,
                                           "Retrieving failed: invalid response.");
        goto out;
    }

    cid = hso_get_cid (MM_MODEM_HSO (serial));
    dns_array = g_array_sized_new (FALSE, TRUE, sizeof (guint32), 2);
    items = g_strsplit (response->str + strlen (OWANDATA_TAG), ", ", 0);

	for (iter = items, i = 0; *iter; iter++, i++) {
		if (i == 0) { /* CID */
			long int tmp;

			errno = 0;
			tmp = strtol (*iter, NULL, 10);
			if (errno != 0 || tmp < 0 || (guint) tmp != cid) {
				info->error = g_error_new (MM_MODEM_ERROR, MM_MODEM_ERROR_GENERAL,
                                           "Unknown CID in OWANDATA response (got %d, expected %d)", (guint) tmp, cid);
				break;
			}
		} else if (i == 1) { /* IP address */
			if (inet_pton (AF_INET, *iter, &tmp) > 0)
                mm_callback_info_set_data (info, "ip4-address", GUINT_TO_POINTER (tmp), NULL);
		} else if (i == 3) { /* DNS 1 */
			if (inet_pton (AF_INET, *iter, &tmp) > 0)
				g_array_append_val (dns_array, tmp);
		} else if (i == 4) { /* DNS 2 */
			if (inet_pton (AF_INET, *iter, &tmp) > 0)
				g_array_append_val (dns_array, tmp);
		}
	}

    g_strfreev (items);
    mm_callback_info_set_data (info, "ip4-dns", dns_array, free_dns_array);

 out:
    mm_callback_info_schedule (info);
}

void
mm_hso_modem_get_ip4_config (MMModemHso *self,
                             MMModemHsoIp4Fn callback,
                             gpointer user_data)
{
    MMCallbackInfo *info;
    char *command;

    g_return_if_fail (MM_IS_MODEM_HSO (self));
    g_return_if_fail (callback != NULL);

    info = mm_callback_info_new_full (MM_MODEM (self), ip4_config_invoke, G_CALLBACK (callback), user_data);
    command = g_strdup_printf ("AT_OWANDATA=%d", hso_get_cid (self));
    mm_serial_queue_command (MM_SERIAL (self), command, 3, get_ip4_config_done, info);
    g_free (command);
}

/*****************************************************************************/

static void
pin_check_done (MMModem *modem, GError *error, gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;

    if (error)
        info->error = g_error_copy (error);
    mm_callback_info_schedule (info);
}

static void
parent_enable_done (MMModem *modem, GError *error, gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;

    if (error) {
        info->error = g_error_copy (error);
        mm_callback_info_schedule (info);
    } else if (GPOINTER_TO_INT (mm_callback_info_get_data (info, "enable")) == FALSE) {
        /* Disable, we're done */
        mm_callback_info_schedule (info);
    } else {
        /* HSO needs manual PIN checking */
        mm_generic_gsm_check_pin (MM_GENERIC_GSM (modem), pin_check_done, info);
    }
}

static void
modem_enable_done (MMSerial *serial,
                   GString *response,
                   GError *error,
                   gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;
    MMModem *parent_modem_iface;

    parent_modem_iface = g_type_interface_peek_parent (MM_MODEM_GET_INTERFACE (serial));
    parent_modem_iface->enable (MM_MODEM (serial),
                                GPOINTER_TO_INT (mm_callback_info_get_data (info, "enable")),
                                parent_enable_done, info);
}

static void
enable (MMModem *modem,
        gboolean enable,
        MMModemFn callback,
        gpointer user_data)
{
    MMCallbackInfo *info;

    info = mm_callback_info_new (modem, callback, user_data);
    mm_callback_info_set_data (info, "enable", GINT_TO_POINTER (enable), NULL);

    if (enable)
        modem_enable_done (MM_SERIAL (modem), NULL, NULL, info);
    else
        mm_serial_queue_command (MM_SERIAL (modem), "AT_OWANCALL=1,0,0", 3, modem_enable_done, info);
}

static void
do_connect (MMModem *modem,
            const char *number,
            MMModemFn callback,
            gpointer user_data)
{
    MMCallbackInfo *info;

    info = mm_callback_info_new (modem, callback, user_data);
    mm_callback_info_schedule (info);
}

/*****************************************************************************/

static void
impl_hso_auth_done (MMModem *modem,
                    GError *error,
                    gpointer user_data)
{
    DBusGMethodInvocation *context = (DBusGMethodInvocation *) user_data;

    if (error)
        dbus_g_method_return_error (context, error);
    else
        dbus_g_method_return (context);
}

static void
impl_hso_authenticate (MMModemHso *self,
                       const char *username,
                       const char *password,
                       DBusGMethodInvocation *context)
{
    /* DBus doesn't support NULLs */
    if (username && strlen (username) == 0)
        username = NULL;
    if (password && strlen (password) == 0)
        password = NULL;

    mm_hso_modem_authenticate (self, username, password, impl_hso_auth_done, context);
}

static void
impl_hso_ip4_config_done (MMModemHso *modem,
                          guint32 address,
                          GArray *dns,
                          GError *error,
                          gpointer user_data)
{
    DBusGMethodInvocation *context = (DBusGMethodInvocation *) user_data;

    if (error)
        dbus_g_method_return_error (context, error);
    else
        dbus_g_method_return (context, address, dns);
}

static void
impl_hso_get_ip4_config (MMModemHso *self,
                         DBusGMethodInvocation *context)
{
    mm_hso_modem_get_ip4_config (self, impl_hso_ip4_config_done, context);
}

static void
connection_enabled (const char *str, gpointer data)
{
    if (str && strlen (str) == 4) {
        if (str[3] == '1')
            connect_pending_done (MM_MODEM_HSO (data));
        if (str[3] == '0')
            /* FIXME: disconnected. do something when we have modem status signals */
            ;
    }
}

static gboolean
hso_parse_response (gpointer data, GString *response, GError **error)
{
    MMModemHsoPrivate *priv = MM_MODEM_HSO_GET_PRIVATE (data);

    mm_util_strip_string (response, priv->connection_enabled_regex, connection_enabled, data);

    return mm_serial_parser_v1_parse (priv->std_parser, response, error);
}

/*****************************************************************************/

static void
mm_modem_hso_init (MMModemHso *self)
{
    MMModemHsoPrivate *priv = MM_MODEM_HSO_GET_PRIVATE (self);

    priv->connection_enabled_regex = g_regex_new ("_OWANCALL: (\\d, \\d)\\r\\n",
                                                  G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    priv->std_parser = (gpointer) mm_serial_parser_v1_new ();
    mm_serial_set_response_parser (MM_SERIAL (self), hso_parse_response, self, NULL);
}

static void
modem_init (MMModem *modem_class)
{
    modem_class->enable = enable;
    modem_class->connect = do_connect;
}

static GObject*
constructor (GType type,
             guint n_construct_params,
             GObjectConstructParam *construct_params)
{
    GObject *object;
    MMModemHsoPrivate *priv;

    object = G_OBJECT_CLASS (mm_modem_hso_parent_class)->constructor (type,
                                                                      n_construct_params,
                                                                      construct_params);
    if (!object)
        return NULL;

    priv = MM_MODEM_HSO_GET_PRIVATE (object);

    if (!priv->network_device) {
        g_warning ("No network device provided");
        g_object_unref (object);
        return NULL;
    }

    return object;
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
    MMModemHsoPrivate *priv = MM_MODEM_HSO_GET_PRIVATE (object);

    switch (prop_id) {
    case PROP_NETWORK_DEVICE:
        /* Construct only */
        priv->network_device = g_value_dup_string (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
    MMModemHsoPrivate *priv = MM_MODEM_HSO_GET_PRIVATE (object);

    switch (prop_id) {
    case PROP_NETWORK_DEVICE:
        g_value_set_string (value, priv->network_device);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}
static void
finalize (GObject *object)
{
    MMModemHsoPrivate *priv = MM_MODEM_HSO_GET_PRIVATE (object);

    /* Clear the pending connection if necessary */
    connect_pending_done (MM_MODEM_HSO (object));

    g_free (priv->network_device);
    g_regex_unref (priv->connection_enabled_regex);
    mm_serial_parser_v1_destroy (priv->std_parser);

    G_OBJECT_CLASS (mm_modem_hso_parent_class)->finalize (object);
}

static void
mm_modem_hso_class_init (MMModemHsoClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    mm_modem_hso_parent_class = g_type_class_peek_parent (klass);
    g_type_class_add_private (object_class, sizeof (MMModemHsoPrivate));

    /* Virtual methods */
    object_class->constructor = constructor;
    object_class->set_property = set_property;
    object_class->get_property = get_property;
    object_class->finalize = finalize;
    /* Properties */
    g_object_class_install_property
        (object_class, PROP_NETWORK_DEVICE,
         g_param_spec_string (MM_MODEM_HSO_NETWORK_DEVICE,
                              "NetworkDevice",
                              "Network device",
                              NULL,
                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

GType
mm_modem_hso_get_type (void)
{
    static GType modem_hso_type = 0;

    if (G_UNLIKELY (modem_hso_type == 0)) {
        static const GTypeInfo modem_hso_type_info = {
            sizeof (MMModemHsoClass),
            (GBaseInitFunc) NULL,
            (GBaseFinalizeFunc) NULL,
            (GClassInitFunc) mm_modem_hso_class_init,
            (GClassFinalizeFunc) NULL,
            NULL,   /* class_data */
            sizeof (MMModemHso),
            0,      /* n_preallocs */
            (GInstanceInitFunc) mm_modem_hso_init,
        };

        static const GInterfaceInfo modem_iface_info = { 
            (GInterfaceInitFunc) modem_init
        };

        modem_hso_type = g_type_register_static (MM_TYPE_GENERIC_GSM, "MMModemHso", &modem_hso_type_info, 0);
        g_type_add_interface_static (modem_hso_type, MM_TYPE_MODEM, &modem_iface_info);

        dbus_g_object_type_install_info (modem_hso_type, &dbus_glib_mm_modem_gsm_hso_object_info);
    }

    return modem_hso_type;
}
