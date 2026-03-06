#include <switch.h>
#include <string.h>
#include <string>
#include <mutex>
#include <list>
#include <algorithm>
#include <condition_variable>
#include <cassert>

#include "mod_audio_fork.h"

// buffer at most 2 secs of audio (at 20 ms packetization)
#define MAX_BUFFERED_MSGS (100)

namespace {
  static int interrupted = 0;
  static struct lws_context *context = NULL;
  static std::list<struct cap_cb *> pendingConnects;
  static std::list<struct cap_cb *> pendingDisconnects;
  static std::list<struct cap_cb *> pendingWrites;
  static std::mutex g_mutex_connects;
  static std::mutex g_mutex_disconnects;
  static std::mutex g_mutex_writes;

  void bufInit(struct cap_cb* cb) {
    cb->buf_head = &cb->audio_buffer[0] + LWS_PRE;
  }
  uint8_t* bufGetWriteHead(struct cap_cb* cb) {
    return cb->buf_head;
  }
  uint8_t* bufGetReadHead(struct cap_cb* cb) {
    return &cb->audio_buffer[0] + LWS_PRE;
  }
  size_t bufGetAvailable(struct cap_cb* cb) {
    uint8_t* pEnd = &cb->audio_buffer[0] + sizeof(cb->audio_buffer);
    assert(cb->buf_head <= pEnd);
    return pEnd - cb->buf_head;
  }
  size_t bufGetUsed(struct cap_cb* cb) {
    return cb->buf_head - &cb->audio_buffer[0] - LWS_PRE;
  }
  uint8_t* bufBumpWriteHead(struct cap_cb* cb, spx_uint32_t len) {
    cb->buf_head += len;
    assert(cb->buf_head <= &cb->audio_buffer[0] + sizeof(cb->audio_buffer));
  }

  /* --- Receive ring buffer helpers (WebSocket → call audio injection) --- */

  void recvBufInit(struct cap_cb* cb) {
    cb->recv_write_pos = 0;
    cb->recv_read_pos = 0;
  }

  size_t recvBufAvailable(struct cap_cb* cb) {
    size_t w = cb->recv_write_pos;
    size_t r = cb->recv_read_pos;
    if (w >= r) return w - r;
    return RECV_BUF_SIZE - r + w;
  }

  size_t recvBufSpace(struct cap_cb* cb) {
    return RECV_BUF_SIZE - 1 - recvBufAvailable(cb);
  }

  size_t recvBufWrite(struct cap_cb* cb, const uint8_t* data, size_t len) {
    size_t space = recvBufSpace(cb);
    if (len > space) len = space;  /* drop excess if full */
    if (len == 0) return 0;

    size_t w = cb->recv_write_pos;
    size_t first = RECV_BUF_SIZE - w;
    if (first >= len) {
      memcpy(cb->recv_buffer + w, data, len);
    } else {
      memcpy(cb->recv_buffer + w, data, first);
      memcpy(cb->recv_buffer, data + first, len - first);
    }
    cb->recv_write_pos = (w + len) % RECV_BUF_SIZE;
    return len;
  }

  size_t recvBufRead(struct cap_cb* cb, uint8_t* out, size_t len) {
    size_t avail = recvBufAvailable(cb);
    if (len > avail) len = avail;
    if (len == 0) return 0;

    size_t r = cb->recv_read_pos;
    size_t first = RECV_BUF_SIZE - r;
    if (first >= len) {
      memcpy(out, cb->recv_buffer + r, len);
    } else {
      memcpy(out, cb->recv_buffer + r, first);
      memcpy(out + first, cb->recv_buffer, len - first);
    }
    cb->recv_read_pos = (r + len) % RECV_BUF_SIZE;
    return len;
  }

  void addPendingConnect(struct cap_cb* cb) {
    std::lock_guard<std::mutex> guard(g_mutex_connects);
    cb->state = LWS_CLIENT_IDLE;
    pendingConnects.push_back(cb);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "addPendingConnect - after adding there are now %d pending\n", pendingConnects.size());
  }

  void addPendingDisconnect(struct cap_cb* cb) {
    std::lock_guard<std::mutex> guard(g_mutex_disconnects);
    cb->state = LWS_CLIENT_DISCONNECTING;
    pendingDisconnects.push_back(cb);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "addPendingDisconnect - after adding there are now %d pending\n", pendingDisconnects.size());
  }

  void addPendingWrite(struct cap_cb* cb) {
    std::lock_guard<std::mutex> guard(g_mutex_writes);
    pendingWrites.push_back(cb);
  }

  struct cap_cb* findAndRemovePendingConnect(struct lws *wsi) {
    struct cap_cb* cb = NULL;
    std::lock_guard<std::mutex> guard(g_mutex_connects);

    for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !cb; ++it) {
      if ((*it)->state == LWS_CLIENT_CONNECTING && (*it)->wsi == wsi) cb = *it;
    }

    if (cb) pendingConnects.remove(cb);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "findAndRemovePendingConnect - after removing there are now %d pending\n", pendingConnects.size());

    return cb;
  }

  void destroy_cb(struct cap_cb *cb) {
    speex_resampler_destroy(cb->resampler);
    switch_mutex_destroy(cb->mutex);
    switch_thread_cond_destroy(cb->cond);
    cb->wsi = NULL;
    cb->state = LWS_CLIENT_DISCONNECTED;
  }

  int connect_client(struct cap_cb* cb, struct lws_per_vhost_data *vhd) {
    struct lws_client_connect_info i;

    memset(&i, 0, sizeof(i));

    i.context = vhd->context;
    i.port = cb->port;
    i.address = cb->host;
    i.path = cb->path;
    i.host = i.address;
    i.origin = i.address;
    i.ssl_connection = cb->sslFlags;
    i.protocol = "audio-fork";
    i.pwsi = &(cb->wsi);

    cb->state = LWS_CLIENT_CONNECTING;
    cb->vhd = vhd;

    if (!lws_client_connect_via_info(&i)) {
      //cb->state = LWS_CLIENT_IDLE;
      return 0;
    }

    return 1;
  }

  static int lws_callback(struct lws *wsi, 
    enum lws_callback_reasons reason,
    void *user, void *in, size_t len) {

    struct lws_per_vhost_data *vhd = 
      (struct lws_per_vhost_data *) lws_protocol_vh_priv_get(lws_get_vhost(wsi), lws_get_protocol(wsi));

    struct lws_vhost* vhost = lws_get_vhost(wsi);
  	struct cap_cb ** pCb = (struct cap_cb **) user;

    switch (reason) {

    case LWS_CALLBACK_PROTOCOL_INIT:
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "lws_callback LWS_CALLBACK_PROTOCOL_INIT wsi: %p\n", wsi);
      vhd = (struct lws_per_vhost_data *) lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi), lws_get_protocol(wsi), sizeof(struct lws_per_vhost_data));
      vhd->context = lws_get_context(wsi);
      vhd->protocol = lws_get_protocol(wsi);
      vhd->vhost = lws_get_vhost(wsi);
      break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      {        
        // check if we have any new connections requested
        {
          std::lock_guard<std::mutex> guard(g_mutex_connects);
          for (auto it = pendingConnects.begin(); it != pendingConnects.end(); ++it) {
            struct cap_cb* cb = *it;
            if (cb->state == LWS_CLIENT_IDLE) {
              connect_client(cb, vhd);
            }
          }
        }

        // process disconnects
        {
          std::lock_guard<std::mutex> guard(g_mutex_disconnects);
          for (auto it = pendingDisconnects.begin(); it != pendingDisconnects.end(); ++it) {
            struct cap_cb* cb = *it;
            lws_callback_on_writable(cb->wsi);
          }
          pendingDisconnects.clear();
        }

        // process writes
        {
          std::lock_guard<std::mutex> guard(g_mutex_writes);
          for (auto it = pendingWrites.begin(); it != pendingWrites.end(); ++it) {
            struct cap_cb* cb = *it;
            lws_callback_on_writable(cb->wsi);
          }
          pendingWrites.clear();
        }

      }
      break;

    /* --- client callbacks --- */
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "lws_callback LWS_CALLBACK_CLIENT_CONNECTION_ERROR wsi: %p\n", wsi);
      {
        struct cap_cb* my_cb = findAndRemovePendingConnect(wsi);
        if (!my_cb) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "lws_callback LWS_CALLBACK_CLIENT_CONNECTION_ERROR unable to find pending connection for wsi: %p\n", wsi);
        }
        else {
          struct cap_cb *cb = *pCb = my_cb;
          switch_mutex_lock(cb->mutex);
          cb->state = LWS_CLIENT_FAILED;
          switch_thread_cond_signal(cb->cond);
          switch_mutex_unlock(cb->mutex);
        }
      }      
      break;


    case LWS_CALLBACK_CLIENT_ESTABLISHED:

      // remove the associated cb from the pending list and allocate audio ring buffer
      {
        struct cap_cb* my_cb = findAndRemovePendingConnect(wsi);
        if (!my_cb) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "lws_callback LWS_CALLBACK_CLIENT_ESTABLISHED unable to find pending connection for wsi: %p\n", wsi);
        }
        else {
          struct cap_cb *cb = *pCb = my_cb;
          switch_mutex_lock(cb->mutex);
          cb->vhd = vhd;
          cb->state = LWS_CLIENT_CONNECTED;
          switch_thread_cond_signal(cb->cond);
          switch_mutex_unlock(cb->mutex);
        }
      }      
      break;

    case LWS_CALLBACK_CLIENT_CLOSED:
      {
        struct cap_cb *cb = *pCb;
        if (cb && cb->state == LWS_CLIENT_DISCONNECTING) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "lws_callback LWS_CALLBACK_CLIENT_CLOSED by us wsi: %p\n", wsi);
          destroy_cb(cb);
        }
        else if (cb && cb->state == LWS_CLIENT_CONNECTED) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "lws_callback LWS_CALLBACK_CLIENT_CLOSED from far end wsi: %p\n", wsi);
          switch_mutex_lock(cb->mutex);
          switch_mutex_unlock(cb->mutex);
          destroy_cb(cb);
        }
      }
      break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
      {
        struct cap_cb *cb = *pCb;
        if (cb->state == LWS_CLIENT_DISCONNECTING) {
          return -1;
        }

        // check for initial metadata
        if (cb->metadata) {          
          int n = cb->metadata_length - LWS_PRE - 1;
          int m = lws_write(wsi, cb->metadata + LWS_PRE, n, LWS_WRITE_TEXT);
          delete[] cb->metadata;
          cb->metadata = NULL;
          cb->metadata_length = 0;
          if (m < n) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error writing metadata %d requested, %d written\n", n, m);
            return -1;
          }          
        }
        else {
          // check for audio packets
          switch_mutex_lock(cb->mutex);
          int n = bufGetUsed(cb);
          if (n > 0) {
            int m = lws_write(wsi, bufGetReadHead(cb), n, LWS_WRITE_BINARY);
            if (m < n) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error writing audio data %d requested, %d written\n", n, m);
              return -1;
            }
            bufInit(cb);
          }
          switch_mutex_unlock(cb->mutex);
        }
        return 0;
      }
      break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
      {
        struct cap_cb *cb = *pCb;
        if (!cb || cb->state != LWS_CLIENT_CONNECTED) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
            "CLIENT_RECEIVE: cb=%p state=%d, ignoring %zu bytes\n", cb, cb ? cb->state : -1, len);
          break;
        }

        int is_binary = lws_frame_is_binary(wsi);
        /* Only accept binary frames (L16 audio); ignore text frames */
        if (!is_binary) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
            "CLIENT_RECEIVE: ignoring text frame (%zu bytes)\n", len);
          break;
        }

        if (len > 0) {
          switch_mutex_lock(cb->mutex);
          size_t written = recvBufWrite(cb, (const uint8_t *)in, len);
          size_t avail = recvBufAvailable(cb);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
            "CLIENT_RECEIVE: got %zu bytes, wrote %zu, buf_avail=%zu\n", len, written, avail);
          if (written < len) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
              "recv buffer overrun: dropped %zu of %zu bytes\n", len - written, len);
          }
          switch_mutex_unlock(cb->mutex);
        }
      }
      break;

    default:
      break;
    }

    return lws_callback_http_dummy(wsi, reason, user, in, len);
  }

  static const struct lws_protocols protocols[] = {
    {
      "audio-fork",
      lws_callback,
      sizeof(void *),
      0,
    },
    { NULL, NULL, 0, 0 }
  };

  void lws_logger(int level, const char *line) {
    switch_log_level_t llevel = SWITCH_LOG_DEBUG;

    switch (level) {
      case LLL_ERR: llevel = SWITCH_LOG_ERROR; break;
      case LLL_WARN: llevel = SWITCH_LOG_WARNING; break;
      case LLL_NOTICE: llevel = SWITCH_LOG_NOTICE; break;
      case LLL_INFO: llevel = SWITCH_LOG_INFO; break;
      break;
    }
	  switch_log_printf(SWITCH_CHANNEL_LOG, llevel, "%s\n", line);

  }

}

extern "C" {
  int parse_ws_uri(const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags) {
    int i = 0, offset;
    char server[MAX_WS_URL_LEN];
    

    // get the scheme
    strncpy(server, szServerUri, MAX_WS_URL_LEN);
    if (0 == strncmp(server, "https://", 8) || 0 == strncmp(server, "HTTPS://", 8)) {
      *pSslFlags = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED;
      offset = 8;
      *pPort = 443;
    }
    else if (0 == strncmp(server, "wss://", 6) || 0 == strncmp(server, "WSS://", 6)) {
      *pSslFlags = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED;
      offset = 6;
      *pPort = 443;
    }
    else if (0 == strncmp(server, "http://", 7) || 0 == strncmp(server, "HTTP://", 7)) {
      offset = 7;
      *pSslFlags = 0;
      *pPort = 80;
    }
    else if (0 == strncmp(server, "ws://", 5) || 0 == strncmp(server, "WS://", 5)) {
      offset = 5;
      *pSslFlags = 0;
      *pPort = 80;
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - error parsing uri %s: invalid scheme\n", szServerUri);;
      return 0;
    }

    // parse host, port and path
    // Format after scheme: host[:port][/path]
    strcpy(path, "/");
    char *p = server + offset;

    // Find the first '/' which separates host[:port] from path
    char *slash = strchr(p, '/');
    if (slash) {
      strncpy(path, slash, MAX_PATH_LEN);
      path[MAX_PATH_LEN - 1] = '\0';
      *slash = '\0';  // terminate host[:port] portion
    }

    // Parse host and optional port from "host" or "host:port"
    char *colon = strchr(p, ':');
    if (colon) {
      *colon = '\0';
      strncpy(host, p, MAX_WS_URL_LEN);
      *pPort = atoi(colon + 1);
    } else {
      strncpy(host, p, MAX_WS_URL_LEN);
    }
    host[MAX_WS_URL_LEN - 1] = '\0';

    return 1;
  }

  switch_status_t fork_init() {
  return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_cleanup() {
      return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_init(switch_core_session_t *session, 
              uint32_t samples_per_second, 
              char *host,
              unsigned int port,
              char *path,
              int sampling,
              int sslFlags,
              int channels,
              char* metadata, 
              void **ppUserData)
  {    	
    switch_channel_t *channel = switch_core_session_get_channel(session);
    struct cap_cb *cb;
    int err;

    // allocate per-session data structure
    cb = (struct cap_cb *) switch_core_session_alloc(session, sizeof(struct cap_cb));
    memset(cb, 0, sizeof(struct cap_cb));
    cb->base = switch_core_session_strdup(session, "mod_audio_fork");
    strncpy(cb->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
    cb->state = LWS_CLIENT_IDLE;
    strncpy(cb->host, host, MAX_WS_URL_LEN);
    cb->port = port;
    strncpy(cb->path, path, MAX_PATH_LEN);
    cb->sslFlags = sslFlags;
    cb->wsi = NULL;
    cb->vhd = NULL;
    cb->metadata = NULL;
    cb->sampling = sampling;
    bufInit(cb);
    recvBufInit(cb);

    switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
    switch_thread_cond_create(&cb->cond, switch_core_session_get_pool(session));

    cb->resampler = speex_resampler_init(channels, 8000, sampling, SWITCH_RESAMPLE_QUALITY, &err);

    if (0 != err) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing resampler: %s.\n", 
        switch_channel_get_name(channel), speex_resampler_strerror(err));
      return SWITCH_STATUS_FALSE;
    }

    // now try to connect
    switch_mutex_lock(cb->mutex);
    addPendingConnect(cb);
    lws_cancel_service(context);
    switch_thread_cond_wait(cb->cond, cb->mutex);
    switch_mutex_unlock(cb->mutex);

    if (cb->state == LWS_CLIENT_FAILED) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: failed connecting to host %s\n", 
        switch_channel_get_name(channel), host);
      destroy_cb(cb);
      return SWITCH_STATUS_FALSE;
    }

    // write initial metadata
    cb->metadata_length = strlen(metadata) + 1 + LWS_PRE;
    cb->metadata = new uint8_t[cb->metadata_length];
    memset(cb->metadata, 0, cb->metadata_length);
    memcpy(cb->metadata + LWS_PRE, metadata, strlen(metadata));
    addPendingWrite(cb);
    lws_cancel_service(cb->vhd->context);

    *ppUserData = cb;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_cleanup(switch_core_session_t *session) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);

    if (bug) {
      struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
      switch_channel_set_private(channel, MY_BUG_NAME, NULL);
      if (cb->wsi) {
        switch_mutex_lock(cb->mutex);
        addPendingDisconnect(cb);
        lws_cancel_service(cb->vhd->context);
        switch_mutex_unlock(cb->mutex);
      }

      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "fork_session_cleanup: Closed stream\n");
      return SWITCH_STATUS_SUCCESS;
    }
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached.\n", switch_channel_get_name(channel));
    return SWITCH_STATUS_FALSE;
  }

  switch_bool_t fork_frame(switch_media_bug_t *bug, void* user_data) {
    switch_core_session_t *session = switch_core_media_bug_get_session(bug);
    struct cap_cb *cb = (struct cap_cb *) user_data;
    bool written = false;
    int channels = switch_core_media_bug_test_flag(bug, SMBF_STEREO) ? 2 : 1;

    if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
      uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
      switch_frame_t frame = {};
      frame.data = data;
      frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

      while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
        if (frame.datalen) {
          size_t n = bufGetAvailable(cb) >> 1;  // divide by 2 to num of uint16_t spaces available
          if (n  > frame.samples) {
            spx_uint32_t out_len = n;
            spx_uint32_t in_len = frame.samples;
            
            speex_resampler_process_interleaved_int(cb->resampler, 
              (const spx_int16_t *) frame.data, 
              (spx_uint32_t *) &in_len, 
              (spx_int16_t *) bufGetWriteHead(cb),
              &out_len);

             // i.e., if we wrote 320 16bit items then we need to increment 320*2 bytes in single-channel mode, twice that in dual channel  
            bufBumpWriteHead(cb, out_len << channels);        
            written = true;
          }
          else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Dropping packet.\n");
          }
        }
      }
      switch_mutex_unlock(cb->mutex);
    }

    if (written) {
      addPendingWrite(cb);
      lws_cancel_service(cb->vhd->context);
    }

    return SWITCH_TRUE;
  }

  static int write_frame_call_count = 0;
  static int write_frame_lock_fail_count = 0;

  switch_bool_t fork_write_frame(switch_media_bug_t *bug, void* user_data) {
    switch_frame_t *frame = switch_core_media_bug_get_write_replace_frame(bug);
    struct cap_cb *cb = (struct cap_cb *) user_data;

    if (!frame || !frame->data || !frame->datalen) return SWITCH_TRUE;

    write_frame_call_count++;

    if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
      size_t avail = recvBufAvailable(cb);

      /* Log periodically and whenever data is available */
      if (write_frame_call_count % 250 == 1 || (avail > 0 && write_frame_call_count % 25 == 0)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
          "fork_write_frame[%d]: avail=%zu frame_datalen=%u lock_fails=%d\n",
          write_frame_call_count, avail, frame->datalen, write_frame_lock_fail_count);
      }

      if (avail >= frame->datalen) {
        /* Fill the frame with received audio */
        recvBufRead(cb, (uint8_t *)frame->data, frame->datalen);
        frame->samples = frame->datalen / 2;  /* L16: 2 bytes per sample */
      } else if (avail > 0) {
        /* Partial fill: read what we have, zero-pad the rest */
        recvBufRead(cb, (uint8_t *)frame->data, avail);
        memset((uint8_t *)frame->data + avail, 0, frame->datalen - avail);
        frame->samples = frame->datalen / 2;
      }
      /* If avail == 0, leave the frame untouched (original silence/audio) */
      switch_mutex_unlock(cb->mutex);
    } else {
      write_frame_lock_fail_count++;
      if (write_frame_lock_fail_count % 50 == 1) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
          "fork_write_frame: mutex trylock FAILED (%d times)\n", write_frame_lock_fail_count);
      }
    }

    switch_core_media_bug_set_write_replace_frame(bug, frame);
    return SWITCH_TRUE;
  }

  switch_status_t fork_service_thread(int *pRunning) {
    struct lws_context_creation_info info;
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE ;
      //LLL_INFO | LLL_PARSER | LLL_HEADER | LLL_EXT | LLL_CLIENT  | LLL_LATENCY | LLL_DEBUG ;

    lws_set_log_level(logs, lws_logger);

    memset(&info, 0, sizeof info); 
    info.port = CONTEXT_PORT_NO_LISTEN; 
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context = lws_create_context(&info);
    if (!context) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_audio_fork: lws_create_context failed\n");
      return SWITCH_STATUS_FALSE;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: successfully created lws context\n");

    int n;
    do {
      n = lws_service(context, 500);
    } while (n >= 0 && *pRunning);

    lws_context_destroy(context);

    return SWITCH_STATUS_FALSE;
  }

}

