/*
   Copyright (C) 2009-2016 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <inttypes.h>
#include <common/generated_server_marshallers.h>

#include "main-channel-client.h"
#include "main-channel.h"
#include "red-channel-client.h"
#include "red-client.h"
#include "reds.h"

#define NET_TEST_WARMUP_BYTES 0
#define NET_TEST_BYTES (1024 * 250)

enum NetTestStage {
    NET_TEST_STAGE_INVALID,
    NET_TEST_STAGE_WARMUP,
    NET_TEST_STAGE_LATENCY,
    NET_TEST_STAGE_RATE,
    NET_TEST_STAGE_COMPLETE,
};

#define CLIENT_CONNECTIVITY_TIMEOUT (MSEC_PER_SEC * 30)
#define PING_INTERVAL (MSEC_PER_SEC * 10)

G_DEFINE_TYPE(MainChannelClient, main_channel_client, RED_TYPE_CHANNEL_CLIENT)

#define MAIN_CHANNEL_CLIENT_PRIVATE(o) \
    (G_TYPE_INSTANCE_GET_PRIVATE((o), TYPE_MAIN_CHANNEL_CLIENT, MainChannelClientPrivate))

struct MainChannelClientPrivate {
    uint32_t connection_id;
    uint32_t ping_id;
    uint32_t net_test_id;
    int net_test_stage;
    uint64_t latency;
    uint64_t bitrate_per_sec;
#ifdef RED_STATISTICS
    SpiceTimer *ping_timer;
    int ping_interval;
#endif
    int mig_wait_connect;
    int mig_connect_ok;
    int mig_wait_prev_complete;
    int mig_wait_prev_try_seamless;
    int init_sent;
    int seamless_mig_dst;
};

typedef struct RedPingPipeItem {
    RedPipeItem base;
    int size;
} RedPingPipeItem;

typedef struct RedTokensPipeItem {
    RedPipeItem base;
    int tokens;
} RedTokensPipeItem;

typedef struct RedAgentDataPipeItem {
    RedPipeItem base;
    uint8_t* data;
    size_t len;
    spice_marshaller_item_free_func free_data;
    void *opaque;
} RedAgentDataPipeItem;

typedef struct RedInitPipeItem {
    RedPipeItem base;
    int connection_id;
    int display_channels_hint;
    int current_mouse_mode;
    int is_client_mouse_allowed;
    int multi_media_time;
    int ram_hint;
} RedInitPipeItem;

typedef struct RedNamePipeItem {
    RedPipeItem base;
    SpiceMsgMainName msg;
} RedNamePipeItem;

typedef struct RedUuidPipeItem {
    RedPipeItem base;
    SpiceMsgMainUuid msg;
} RedUuidPipeItem;

typedef struct RedNotifyPipeItem {
    RedPipeItem base;
    char *msg;
} RedNotifyPipeItem;

typedef struct RedMouseModePipeItem {
    RedPipeItem base;
    int current_mode;
    int is_client_mouse_allowed;
} RedMouseModePipeItem;

typedef struct RedMultiMediaTimePipeItem {
    RedPipeItem base;
    int time;
} RedMultiMediaTimePipeItem;

#define ZERO_BUF_SIZE 4096

static const uint8_t zero_page[ZERO_BUF_SIZE] = {0};

enum {
    PROP0,
    PROP_CONNECTION_ID
};

static void main_channel_client_get_property(GObject *object,
                                             guint property_id,
                                             GValue *value,
                                             GParamSpec *pspec)
{
    MainChannelClient *self = MAIN_CHANNEL_CLIENT(object);

    switch (property_id)
    {
        case PROP_CONNECTION_ID:
            g_value_set_uint(value, self->priv->connection_id);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void main_channel_client_set_property(GObject *object,
                                             guint property_id,
                                             const GValue *value,
                                             GParamSpec *pspec)
{
    MainChannelClient *self = MAIN_CHANNEL_CLIENT(object);

    switch (property_id)
    {
        case PROP_CONNECTION_ID:
            self->priv->connection_id = g_value_get_uint(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

#ifdef RED_STATISTICS
static void ping_timer_cb(void *opaque);
#endif

static void main_channel_client_constructed(GObject *object)
{
    G_OBJECT_CLASS(main_channel_client_parent_class)->constructed(object);
#ifdef RED_STATISTICS
    MainChannelClient *self = MAIN_CHANNEL_CLIENT(object);
    RedsState *reds =
        red_channel_get_server(red_channel_client_get_channel(RED_CHANNEL_CLIENT(object)));

    self->priv->ping_timer = reds_core_timer_add(reds, ping_timer_cb, self);
    if (!self->priv->ping_timer) {
        spice_error("ping timer create failed");
    }
    self->priv->ping_interval = PING_INTERVAL;
#endif
}

static void main_channel_client_finalize(GObject *object)
{
#ifdef RED_STATISTICS
    MainChannelClient *self = MAIN_CHANNEL_CLIENT(object);
    RedsState *reds =
        red_channel_get_server(red_channel_client_get_channel(RED_CHANNEL_CLIENT(object)));

    reds_core_timer_remove(reds, self->priv->ping_timer);
#endif
    G_OBJECT_CLASS(main_channel_client_parent_class)->finalize(object);
}

static void main_channel_client_class_init(MainChannelClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(MainChannelClientPrivate));

    object_class->get_property = main_channel_client_get_property;
    object_class->set_property = main_channel_client_set_property;
    object_class->finalize = main_channel_client_finalize;
    object_class->constructed = main_channel_client_constructed;

    g_object_class_install_property(object_class,
                                    PROP_CONNECTION_ID,
                                    g_param_spec_uint("connection-id",
                                                      "Connection ID",
                                                      "Connection ID",
                                                      0,
                                                      G_MAXUINT,
                                                      0,
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));
}

static void main_channel_client_init(MainChannelClient *self)
{
    self->priv = MAIN_CHANNEL_CLIENT_PRIVATE(self);
    self->priv->bitrate_per_sec = ~0;
}

static int main_channel_client_push_ping(MainChannelClient *mcc, int size);

static void main_notify_item_free(RedPipeItem *base)
{
    RedNotifyPipeItem *data = SPICE_UPCAST(RedNotifyPipeItem, base);
    free(data->msg);
    free(data);
}

static RedPipeItem *main_notify_item_new(const char *msg, int num)
{
    RedNotifyPipeItem *item = spice_malloc(sizeof(RedNotifyPipeItem));

    red_pipe_item_init_full(&item->base, RED_PIPE_ITEM_TYPE_MAIN_NOTIFY,
                            main_notify_item_free);
    item->msg = spice_strdup(msg);
    return &item->base;
}

void main_channel_client_start_net_test(MainChannelClient *mcc, int test_rate)
{
    if (!mcc || mcc->priv->net_test_id) {
        return;
    }

    if (!test_rate) {
        red_channel_client_start_connectivity_monitoring(RED_CHANNEL_CLIENT(mcc),
                                                         CLIENT_CONNECTIVITY_TIMEOUT);
        return;
    }

    main_channel_client_push_ping(mcc, NET_TEST_WARMUP_BYTES);
    main_channel_client_push_ping(mcc, 0);
    main_channel_client_push_ping(mcc, NET_TEST_BYTES);

    mcc->priv->net_test_id = mcc->priv->ping_id - 2;
    mcc->priv->net_test_stage = NET_TEST_STAGE_WARMUP;
}

static RedPipeItem *red_ping_item_new(int size)
{
    RedPingPipeItem *item = spice_malloc(sizeof(RedPingPipeItem));

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_PING);
    item->size = size;
    return &item->base;
}

static int main_channel_client_push_ping(MainChannelClient *mcc, int size)
{
    RedPipeItem *item;

    if (mcc == NULL) {
        return FALSE;
    }
    item = red_ping_item_new(size);
    red_channel_client_pipe_add_push(RED_CHANNEL_CLIENT(mcc), item);
    return TRUE;
}

static RedPipeItem *main_agent_tokens_item_new(uint32_t num_tokens)
{
    RedTokensPipeItem *item = spice_malloc(sizeof(RedTokensPipeItem));

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_AGENT_TOKEN);
    item->tokens = num_tokens;
    return &item->base;
}


void main_channel_client_push_agent_tokens(MainChannelClient *mcc, uint32_t num_tokens)
{
    RedPipeItem *item = main_agent_tokens_item_new(num_tokens);

    red_channel_client_pipe_add_push(RED_CHANNEL_CLIENT(mcc), item);
}

static void main_agent_data_item_free(RedPipeItem *base)
{
    RedAgentDataPipeItem *item = SPICE_UPCAST(RedAgentDataPipeItem, base);
    item->free_data(item->data, item->opaque);
    free(item);
}

static RedPipeItem *main_agent_data_item_new(uint8_t* data, size_t len,
                                             spice_marshaller_item_free_func free_data,
                                             void *opaque)
{
    RedAgentDataPipeItem *item = spice_malloc(sizeof(RedAgentDataPipeItem));

    red_pipe_item_init_full(&item->base, RED_PIPE_ITEM_TYPE_MAIN_AGENT_DATA,
                            main_agent_data_item_free);
    item->data = data;
    item->len = len;
    item->free_data = free_data;
    item->opaque = opaque;
    return &item->base;
}

void main_channel_client_push_agent_data(MainChannelClient *mcc, uint8_t* data, size_t len,
           spice_marshaller_item_free_func free_data, void *opaque)
{
    RedPipeItem *item;

    item = main_agent_data_item_new(data, len, free_data, opaque);
    red_channel_client_pipe_add_push(RED_CHANNEL_CLIENT(mcc), item);
}

static RedPipeItem *main_init_item_new(int connection_id,
                                       int display_channels_hint,
                                       int current_mouse_mode,
                                       int is_client_mouse_allowed,
                                       int multi_media_time,
                                       int ram_hint)
{
    RedInitPipeItem *item = spice_malloc(sizeof(RedInitPipeItem));

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_INIT);
    item->connection_id = connection_id;
    item->display_channels_hint = display_channels_hint;
    item->current_mouse_mode = current_mouse_mode;
    item->is_client_mouse_allowed = is_client_mouse_allowed;
    item->multi_media_time = multi_media_time;
    item->ram_hint = ram_hint;
    return &item->base;
}

void main_channel_client_push_init(MainChannelClient *mcc,
                                   int display_channels_hint,
                                   int current_mouse_mode,
                                   int is_client_mouse_allowed,
                                   int multi_media_time,
                                   int ram_hint)
{
    RedPipeItem *item;

    item = main_init_item_new(mcc->priv->connection_id, display_channels_hint,
                              current_mouse_mode, is_client_mouse_allowed,
                              multi_media_time, ram_hint);
    red_channel_client_pipe_add_push(RED_CHANNEL_CLIENT(mcc), item);
}

static RedPipeItem *main_name_item_new(const char *name)
{
    RedNamePipeItem *item = spice_malloc(sizeof(RedNamePipeItem) + strlen(name) + 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_NAME);
    item->msg.name_len = strlen(name) + 1;
    memcpy(&item->msg.name, name, item->msg.name_len);

    return &item->base;
}

void main_channel_client_push_name(MainChannelClient *mcc, const char *name)
{
    RedPipeItem *item;

    if (!red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(mcc),
                                            SPICE_MAIN_CAP_NAME_AND_UUID))
        return;

    item = main_name_item_new(name);
    red_channel_client_pipe_add_push(RED_CHANNEL_CLIENT(mcc), item);
}

static RedPipeItem *main_uuid_item_new(const uint8_t uuid[16])
{
    RedUuidPipeItem *item = spice_malloc(sizeof(RedUuidPipeItem));

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_UUID);
    memcpy(item->msg.uuid, uuid, sizeof(item->msg.uuid));

    return &item->base;
}

void main_channel_client_push_uuid(MainChannelClient *mcc, const uint8_t uuid[16])
{
    RedPipeItem *item;

    if (!red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(mcc),
                                            SPICE_MAIN_CAP_NAME_AND_UUID))
        return;

    item = main_uuid_item_new(uuid);
    red_channel_client_pipe_add_push(RED_CHANNEL_CLIENT(mcc), item);
}

void main_channel_client_push_notify(MainChannelClient *mcc, const char *msg)
{
    RedPipeItem *item = main_notify_item_new(msg, 1);
    red_channel_client_pipe_add_push(RED_CHANNEL_CLIENT(mcc), item);
}

RedPipeItem *main_mouse_mode_item_new(RedChannelClient *rcc, void *data, int num)
{
    RedMouseModePipeItem *item = spice_malloc(sizeof(RedMouseModePipeItem));
    MainMouseModeItemInfo *info = data;

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_MOUSE_MODE);
    item->current_mode = info->current_mode;
    item->is_client_mouse_allowed = info->is_client_mouse_allowed;
    return &item->base;
}

RedPipeItem *main_multi_media_time_item_new(RedChannelClient *rcc,
                                            void *data, int num)
{
    MainMultiMediaTimeItemInfo *info = data;
    RedMultiMediaTimePipeItem *item;

    item = spice_malloc(sizeof(RedMultiMediaTimePipeItem));
    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_MULTI_MEDIA_TIME);
    item->time = info->time;
    return &item->base;
}

void main_channel_client_handle_migrate_connected(MainChannelClient *mcc,
                                                  int success,
                                                  int seamless)
{
    RedClient *client = red_channel_client_get_client(RED_CHANNEL_CLIENT(mcc));
    spice_printerr("client %p connected: %d seamless %d", client, success, seamless);
    if (mcc->priv->mig_wait_connect) {
        RedChannel *channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(mcc));
        MainChannel *main_channel = MAIN_CHANNEL(channel);

        mcc->priv->mig_wait_connect = FALSE;
        mcc->priv->mig_connect_ok = success;
        main_channel_on_migrate_connected(main_channel, success, seamless);
    } else {
        if (success) {
            spice_printerr("client %p MIGRATE_CANCEL", client);
            red_channel_client_pipe_add_empty_msg(RED_CHANNEL_CLIENT(mcc),
                                                  SPICE_MSG_MAIN_MIGRATE_CANCEL);
        }
    }
}

void main_channel_client_handle_migrate_dst_do_seamless(MainChannelClient *mcc,
                                                        uint32_t src_version)
{
    RedChannel *channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(mcc));
    if (reds_on_migrate_dst_set_seamless(red_channel_get_server(channel), mcc, src_version)) {
        mcc->priv->seamless_mig_dst = TRUE;
        red_channel_client_pipe_add_empty_msg(RED_CHANNEL_CLIENT(mcc),
                                             SPICE_MSG_MAIN_MIGRATE_DST_SEAMLESS_ACK);
    } else {
        red_channel_client_pipe_add_empty_msg(RED_CHANNEL_CLIENT(mcc),
                                              SPICE_MSG_MAIN_MIGRATE_DST_SEAMLESS_NACK);
    }
}
void main_channel_client_handle_pong(MainChannelClient *mcc, SpiceMsgPing *ping, uint32_t size)
{
    uint64_t roundtrip;
    RedChannelClient* rcc = RED_CHANNEL_CLIENT(mcc);

    roundtrip = g_get_monotonic_time() - ping->timestamp;

    if (ping->id != mcc->priv->net_test_id) {
        /*
         * channel client monitors the connectivity using ping-pong messages
         */
        red_channel_client_handle_message(rcc, size, SPICE_MSGC_PONG, ping);
        return;
    }

    switch (mcc->priv->net_test_stage) {
    case NET_TEST_STAGE_WARMUP:
        mcc->priv->net_test_id++;
        mcc->priv->net_test_stage = NET_TEST_STAGE_LATENCY;
        mcc->priv->latency = roundtrip;
        break;
    case NET_TEST_STAGE_LATENCY:
        mcc->priv->net_test_id++;
        mcc->priv->net_test_stage = NET_TEST_STAGE_RATE;
        mcc->priv->latency = MIN(mcc->priv->latency, roundtrip);
        break;
    case NET_TEST_STAGE_RATE:
        mcc->priv->net_test_id = 0;
        if (roundtrip <= mcc->priv->latency) {
            // probably high load on client or server result with incorrect values
            spice_printerr("net test: invalid values, latency %" PRIu64
                           " roundtrip %" PRIu64 ". assuming high"
                           "bandwidth", mcc->priv->latency, roundtrip);
            mcc->priv->latency = 0;
            mcc->priv->net_test_stage = NET_TEST_STAGE_INVALID;
            red_channel_client_start_connectivity_monitoring(RED_CHANNEL_CLIENT(mcc),
                                                             CLIENT_CONNECTIVITY_TIMEOUT);
            break;
        }
        mcc->priv->bitrate_per_sec = (uint64_t)(NET_TEST_BYTES * 8) * 1000000
            / (roundtrip - mcc->priv->latency);
        mcc->priv->net_test_stage = NET_TEST_STAGE_COMPLETE;
        spice_printerr("net test: latency %f ms, bitrate %"PRIu64" bps (%f Mbps)%s",
                       (double)mcc->priv->latency / 1000,
                       mcc->priv->bitrate_per_sec,
                       (double)mcc->priv->bitrate_per_sec / 1024 / 1024,
                       main_channel_client_is_low_bandwidth(mcc) ? " LOW BANDWIDTH" : "");
        red_channel_client_start_connectivity_monitoring(RED_CHANNEL_CLIENT(mcc),
                                                         CLIENT_CONNECTIVITY_TIMEOUT);
        break;
    default:
        spice_printerr("invalid net test stage, ping id %d test id %d stage %d",
                       ping->id,
                       mcc->priv->net_test_id,
                       mcc->priv->net_test_stage);
        mcc->priv->net_test_stage = NET_TEST_STAGE_INVALID;
    }
}

void main_channel_client_handle_migrate_end(MainChannelClient *mcc)
{
    RedClient *client = red_channel_client_get_client(RED_CHANNEL_CLIENT(mcc));
    if (!red_client_during_migrate_at_target(client)) {
        spice_printerr("unexpected SPICE_MSGC_MIGRATE_END");
        return;
    }
    if (!red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(mcc),
                                            SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE)) {
        spice_printerr("unexpected SPICE_MSGC_MIGRATE_END, "
                   "client does not support semi-seamless migration");
            return;
    }
    red_client_semi_seamless_migrate_complete(client);
}

void main_channel_client_migrate_cancel_wait(MainChannelClient *mcc)
{
    if (mcc->priv->mig_wait_connect) {
        spice_printerr("client %p cancel wait connect",
                       red_channel_client_get_client(RED_CHANNEL_CLIENT(mcc)));
        mcc->priv->mig_wait_connect = FALSE;
        mcc->priv->mig_connect_ok = FALSE;
    }
    mcc->priv->mig_wait_prev_complete = FALSE;
}

void main_channel_client_migrate_dst_complete(MainChannelClient *mcc)
{
    if (mcc->priv->mig_wait_prev_complete) {
        if (mcc->priv->mig_wait_prev_try_seamless) {
            RedChannel *channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(mcc));
            spice_assert(red_channel_get_n_clients(channel) == 1);
            red_channel_client_pipe_add_type(RED_CHANNEL_CLIENT(mcc),
                                             RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS);
        } else {
            red_channel_client_pipe_add_type(RED_CHANNEL_CLIENT(mcc),
                                             RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN);
        }
        mcc->priv->mig_wait_connect = TRUE;
        mcc->priv->mig_wait_prev_complete = FALSE;
    }
}

gboolean main_channel_client_migrate_src_complete(MainChannelClient *mcc,
                                                  gboolean success)
{
    gboolean ret = FALSE;
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(mcc);
    RedClient *client = red_channel_client_get_client(rcc);
    int semi_seamless_support = red_channel_client_test_remote_cap(rcc,
                                                                   SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE);
    if (semi_seamless_support && mcc->priv->mig_connect_ok) {
        if (success) {
            spice_printerr("client %p MIGRATE_END", client);
            red_channel_client_pipe_add_empty_msg(rcc,
                                                  SPICE_MSG_MAIN_MIGRATE_END);
            ret = TRUE;
        } else {
            spice_printerr("client %p MIGRATE_CANCEL", client);
            red_channel_client_pipe_add_empty_msg(rcc,
                                                  SPICE_MSG_MAIN_MIGRATE_CANCEL);
        }
    } else {
        if (success) {
            spice_printerr("client %p SWITCH_HOST", client);
            red_channel_client_pipe_add_type(rcc,
                                             RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST);
        }
    }
    mcc->priv->mig_connect_ok = FALSE;
    mcc->priv->mig_wait_connect = FALSE;

    return ret;
}

#ifdef RED_STATISTICS
static void do_ping_client(MainChannelClient *mcc,
    const char *opt, int has_interval, int interval)
{
    RedChannel *channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(mcc));
    spice_printerr("");
    if (!opt) {
        main_channel_client_push_ping(mcc, 0);
    } else if (!strcmp(opt, "on")) {
        if (has_interval && interval > 0) {
            mcc->priv->ping_interval = interval * MSEC_PER_SEC;
        }
        reds_core_timer_start(red_channel_get_server(channel),
                              mcc->priv->ping_timer, mcc->priv->ping_interval);
    } else if (!strcmp(opt, "off")) {
        reds_core_timer_cancel(red_channel_get_server(channel),
                               mcc->priv->ping_timer);
    } else {
        return;
    }
}

static void ping_timer_cb(void *opaque)
{
    MainChannelClient *mcc = opaque;
    RedChannel *channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(mcc));

    if (!red_channel_client_is_connected(RED_CHANNEL_CLIENT(mcc))) {
        spice_printerr("not connected to peer, ping off");
        reds_core_timer_cancel(red_channel_get_server(channel),
                               mcc->priv->ping_timer);
        return;
    }
    do_ping_client(mcc, NULL, 0, 0);
    reds_core_timer_start(red_channel_get_server(channel),
                          mcc->priv->ping_timer, mcc->priv->ping_interval);
}
#endif /* RED_STATISTICS */

MainChannelClient *main_channel_client_create(MainChannel *main_chan, RedClient *client,
                                              RedsStream *stream, uint32_t connection_id,
                                              int num_common_caps, uint32_t *common_caps,
                                              int num_caps, uint32_t *caps)
{
    MainChannelClient *mcc;
    GArray *common_caps_array = NULL, *caps_array = NULL;

    if (common_caps) {
        common_caps_array = g_array_sized_new(FALSE, FALSE, sizeof (*common_caps),
                                              num_common_caps);
        g_array_append_vals(common_caps_array, common_caps, num_common_caps);
    }
    if (caps) {
        caps_array = g_array_sized_new(FALSE, FALSE, sizeof (*caps), num_caps);
        g_array_append_vals(caps_array, caps, num_caps);
    }

    mcc = g_initable_new(TYPE_MAIN_CHANNEL_CLIENT,
                         NULL, NULL,
                         "channel", RED_CHANNEL(main_chan),
                         "client", client,
                         "stream", stream,
                         "monitor-latency", FALSE,
                         "caps", caps_array,
                         "common-caps", common_caps_array,
                         "connection-id", connection_id,
                         NULL);

    if (caps_array)
        g_array_unref(caps_array);
    if (common_caps_array)
        g_array_unref(common_caps_array);

    return mcc;
}

int main_channel_client_is_network_info_initialized(MainChannelClient *mcc)
{
    return mcc->priv->net_test_stage == NET_TEST_STAGE_COMPLETE;
}

int main_channel_client_is_low_bandwidth(MainChannelClient *mcc)
{
    // TODO: configurable?
    return mcc->priv->bitrate_per_sec < 10 * 1024 * 1024;
}

uint64_t main_channel_client_get_bitrate_per_sec(MainChannelClient *mcc)
{
    return mcc->priv->bitrate_per_sec;
}

uint64_t main_channel_client_get_roundtrip_ms(MainChannelClient *mcc)
{
    return mcc->priv->latency / 1000;
}

void main_channel_client_migrate(RedChannelClient *rcc)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    reds_on_main_channel_migrate(red_channel_get_server(channel),
                                 MAIN_CHANNEL_CLIENT(rcc));
    red_channel_client_default_migrate(rcc);
}

gboolean main_channel_client_connect_semi_seamless(MainChannelClient *mcc)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(mcc);
    if (red_channel_client_test_remote_cap(rcc,
                                           SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE)) {
        RedClient *client = red_channel_client_get_client(rcc);
        if (red_client_during_migrate_at_target(client)) {
            spice_printerr("client %p: wait till previous migration completes", client);
            mcc->priv->mig_wait_prev_complete = TRUE;
            mcc->priv->mig_wait_prev_try_seamless = FALSE;
        } else {
            red_channel_client_pipe_add_type(rcc,
                                             RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN);
            mcc->priv->mig_wait_connect = TRUE;
        }
        mcc->priv->mig_connect_ok = FALSE;
        return TRUE;
    }
    return FALSE;
}

void main_channel_client_connect_seamless(MainChannelClient *mcc)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(mcc);
    RedClient *client = red_channel_client_get_client(rcc);
    spice_assert(red_channel_client_test_remote_cap(rcc,
                                                    SPICE_MAIN_CAP_SEAMLESS_MIGRATE));
    if (red_client_during_migrate_at_target(client)) {
        spice_printerr("client %p: wait till previous migration completes", client);
        mcc->priv->mig_wait_prev_complete = TRUE;
        mcc->priv->mig_wait_prev_try_seamless = TRUE;
    } else {
        red_channel_client_pipe_add_type(rcc,
                                         RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS);
        mcc->priv->mig_wait_connect = TRUE;
    }
    mcc->priv->mig_connect_ok = FALSE;
}

uint32_t main_channel_client_get_connection_id(MainChannelClient *mcc)
{
    return mcc->priv->connection_id;
}

static uint32_t main_channel_client_next_ping_id(MainChannelClient *mcc)
{
    return ++mcc->priv->ping_id;
}

static void main_channel_marshall_channels(RedChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           RedPipeItem *item)
{
    SpiceMsgChannels* channels_info;
    RedChannel *channel = red_channel_client_get_channel(rcc);

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_CHANNELS_LIST, item);
    channels_info = reds_msg_channels_new(red_channel_get_server(channel));
    spice_marshall_msg_main_channels_list(m, channels_info);
    free(channels_info);
}

static void main_channel_marshall_ping(RedChannelClient *rcc,
                                       SpiceMarshaller *m,
                                       RedPingPipeItem *item)
{
    MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
    SpiceMsgPing ping;
    int size_left = item->size;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PING, &item->base);
    ping.id = main_channel_client_next_ping_id(mcc);
    ping.timestamp = g_get_monotonic_time();
    spice_marshall_msg_ping(m, &ping);

    while (size_left > 0) {
        int now = MIN(ZERO_BUF_SIZE, size_left);
        size_left -= now;
        spice_marshaller_add_by_ref(m, zero_page, now);
    }
}

static void main_channel_marshall_mouse_mode(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             RedMouseModePipeItem *item)
{
    SpiceMsgMainMouseMode mouse_mode;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_MOUSE_MODE, &item->base);
    mouse_mode.supported_modes = SPICE_MOUSE_MODE_SERVER;
    if (item->is_client_mouse_allowed) {
        mouse_mode.supported_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    mouse_mode.current_mode = item->current_mode;
    spice_marshall_msg_main_mouse_mode(m, &mouse_mode);
}

static void main_channel_marshall_agent_disconnected(RedChannelClient *rcc,
                                                     SpiceMarshaller *m,
                                                     RedPipeItem *item)
{
    SpiceMsgMainAgentDisconnect disconnect;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_AGENT_DISCONNECTED, item);
    disconnect.error_code = SPICE_LINK_ERR_OK;
    spice_marshall_msg_main_agent_disconnected(m, &disconnect);
}

static void main_channel_marshall_tokens(RedChannelClient *rcc,
                                         SpiceMarshaller *m, RedTokensPipeItem *item)
{
    SpiceMsgMainAgentTokens tokens;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_AGENT_TOKEN, &item->base);
    tokens.num_tokens = item->tokens;
    spice_marshall_msg_main_agent_token(m, &tokens);
}

static void main_channel_marshall_agent_data(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             RedAgentDataPipeItem *item)
{
    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_AGENT_DATA, &item->base);
    spice_marshaller_add_by_ref(m, item->data, item->len);
}

static void main_channel_marshall_migrate_data_item(RedChannelClient *rcc,
                                                    SpiceMarshaller *m,
                                                    RedPipeItem *item)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA, item);
    // TODO: from reds split. ugly separation.
    reds_marshall_migrate_data(red_channel_get_server(channel), m);
}

static void main_channel_marshall_init(RedChannelClient *rcc,
                                       SpiceMarshaller *m,
                                       RedInitPipeItem *item)
{
    SpiceMsgMainInit init; // TODO - remove this copy, make RedInitPipeItem reuse SpiceMsgMainInit
    RedChannel *channel = red_channel_client_get_channel(rcc);

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_INIT, &item->base);
    init.session_id = item->connection_id;
    init.display_channels_hint = item->display_channels_hint;
    init.current_mouse_mode = item->current_mouse_mode;
    init.supported_mouse_modes = SPICE_MOUSE_MODE_SERVER;
    if (item->is_client_mouse_allowed) {
        init.supported_mouse_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    init.agent_connected = reds_has_vdagent(red_channel_get_server(channel));
    init.agent_tokens = REDS_AGENT_WINDOW_SIZE;
    init.multi_media_time = item->multi_media_time;
    init.ram_hint = item->ram_hint;
    spice_marshall_msg_main_init(m, &init);
}

static void main_channel_marshall_notify(RedChannelClient *rcc,
                                         SpiceMarshaller *m, RedNotifyPipeItem *item)
{
    SpiceMsgNotify notify;

    red_channel_client_init_send_data(rcc, SPICE_MSG_NOTIFY, &item->base);
    notify.time_stamp = spice_get_monotonic_time_ns(); // TODO - move to main_new_notify_item
    notify.severity = SPICE_NOTIFY_SEVERITY_WARN;
    notify.visibilty = SPICE_NOTIFY_VISIBILITY_HIGH;
    notify.what = SPICE_WARN_GENERAL;
    notify.message_len = strlen(item->msg);
    spice_marshall_msg_notify(m, &notify);
    spice_marshaller_add(m, (uint8_t *)item->msg, notify.message_len + 1);
}

static void main_channel_fill_migrate_dst_info(MainChannel *main_channel,
                                               SpiceMigrationDstInfo *dst_info)
{
    const RedsMigSpice *mig_dst = main_channel_get_migration_target(main_channel);
    dst_info->port = mig_dst->port;
    dst_info->sport = mig_dst->sport;
    dst_info->host_size = strlen(mig_dst->host) + 1;
    dst_info->host_data = (uint8_t *)mig_dst->host;
    if (mig_dst->cert_subject) {
        dst_info->cert_subject_size = strlen(mig_dst->cert_subject) + 1;
        dst_info->cert_subject_data = (uint8_t *)mig_dst->cert_subject;
    } else {
        dst_info->cert_subject_size = 0;
        dst_info->cert_subject_data = NULL;
    }
}

static void main_channel_marshall_migrate_begin(SpiceMarshaller *m, RedChannelClient *rcc,
                                                RedPipeItem *item)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    SpiceMsgMainMigrationBegin migrate;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_MIGRATE_BEGIN, item);
    main_channel_fill_migrate_dst_info(MAIN_CHANNEL(channel), &migrate.dst_info);
    spice_marshall_msg_main_migrate_begin(m, &migrate);
}

static void main_channel_marshall_migrate_begin_seamless(SpiceMarshaller *m,
                                                         RedChannelClient *rcc,
                                                         RedPipeItem *item)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    SpiceMsgMainMigrateBeginSeamless migrate_seamless;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_MIGRATE_BEGIN_SEAMLESS, item);
    main_channel_fill_migrate_dst_info(MAIN_CHANNEL(channel), &migrate_seamless.dst_info);
    migrate_seamless.src_mig_version = SPICE_MIGRATION_PROTOCOL_VERSION;
    spice_marshall_msg_main_migrate_begin_seamless(m, &migrate_seamless);
}

static void main_channel_marshall_multi_media_time(RedChannelClient *rcc,
                                                   SpiceMarshaller *m,
                                                   RedMultiMediaTimePipeItem *item)
{
    SpiceMsgMainMultiMediaTime time_mes;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_MULTI_MEDIA_TIME, &item->base);
    time_mes.time = item->time;
    spice_marshall_msg_main_multi_media_time(m, &time_mes);
}

static void main_channel_marshall_migrate_switch(SpiceMarshaller *m, RedChannelClient *rcc,
                                                 RedPipeItem *item)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    SpiceMsgMainMigrationSwitchHost migrate;
    MainChannel *main_ch;
    const RedsMigSpice *mig_target;

    spice_printerr("");
    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_MIGRATE_SWITCH_HOST, item);
    main_ch = MAIN_CHANNEL(channel);
    mig_target = main_channel_get_migration_target(main_ch);
    migrate.port = mig_target->port;
    migrate.sport = mig_target->sport;
    migrate.host_size = strlen(mig_target->host) + 1;
    migrate.host_data = (uint8_t *)mig_target->host;
    if (mig_target->cert_subject) {
        migrate.cert_subject_size = strlen(mig_target->cert_subject) + 1;
        migrate.cert_subject_data = (uint8_t *)mig_target->cert_subject;
    } else {
        migrate.cert_subject_size = 0;
        migrate.cert_subject_data = NULL;
    }
    spice_marshall_msg_main_migrate_switch_host(m, &migrate);
}

static void main_channel_marshall_agent_connected(SpiceMarshaller *m,
                                                  RedChannelClient *rcc,
                                                  RedPipeItem *item)
{
    SpiceMsgMainAgentConnectedTokens connected;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS, item);
    connected.num_tokens = REDS_AGENT_WINDOW_SIZE;
    spice_marshall_msg_main_agent_connected_tokens(m, &connected);
}

void main_channel_client_send_item(RedChannelClient *rcc, RedPipeItem *base)
{
    MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    /* In semi-seamless migration (dest side), the connection is started from scratch, and
     * we ignore any pipe item that arrives before the INIT msg is sent.
     * For seamless we don't send INIT, and the connection continues from the same place
     * it stopped on the src side. */
    if (!mcc->priv->init_sent &&
        !mcc->priv->seamless_mig_dst &&
        base->type != RED_PIPE_ITEM_TYPE_MAIN_INIT) {
        spice_printerr("Init msg for client %p was not sent yet "
                       "(client is probably during semi-seamless migration). Ignoring msg type %d",
                       red_channel_client_get_client(rcc), base->type);
        return;
    }
    switch (base->type) {
        case RED_PIPE_ITEM_TYPE_MAIN_CHANNELS_LIST:
            main_channel_marshall_channels(rcc, m, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_PING:
            main_channel_marshall_ping(rcc, m,
                SPICE_UPCAST(RedPingPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MOUSE_MODE:
            main_channel_marshall_mouse_mode(rcc, m,
                SPICE_UPCAST(RedMouseModePipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_DISCONNECTED:
            main_channel_marshall_agent_disconnected(rcc, m, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_TOKEN:
            main_channel_marshall_tokens(rcc, m,
                SPICE_UPCAST(RedTokensPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_DATA:
            main_channel_marshall_agent_data(rcc, m,
                SPICE_UPCAST(RedAgentDataPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_DATA:
            main_channel_marshall_migrate_data_item(rcc, m, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_INIT:
            mcc->priv->init_sent = TRUE;
            main_channel_marshall_init(rcc, m,
                SPICE_UPCAST(RedInitPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_NOTIFY:
            main_channel_marshall_notify(rcc, m,
                SPICE_UPCAST(RedNotifyPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN:
            main_channel_marshall_migrate_begin(m, rcc, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS:
            main_channel_marshall_migrate_begin_seamless(m, rcc, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MULTI_MEDIA_TIME:
            main_channel_marshall_multi_media_time(rcc, m,
                SPICE_UPCAST(RedMultiMediaTimePipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST:
            main_channel_marshall_migrate_switch(m, rcc, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_NAME:
            red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_NAME, base);
            spice_marshall_msg_main_name(m, &SPICE_UPCAST(RedNamePipeItem, base)->msg);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_UUID:
            red_channel_client_init_send_data(rcc, SPICE_MSG_MAIN_UUID, base);
            spice_marshall_msg_main_uuid(m, &SPICE_UPCAST(RedUuidPipeItem, base)->msg);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_CONNECTED_TOKENS:
            main_channel_marshall_agent_connected(m, rcc, base);
            break;
        default:
            break;
    };
    red_channel_client_begin_send_message(rcc);
}
