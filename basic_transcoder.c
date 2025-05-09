#define G_LOG_DOMAIN "TranscoderApp"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>  // For getrusage

#include <glib.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <gst/gsterror.h> // For error domains like G_IO_ERROR
#include <gst/pbutils/pbutils.h> // For GstDiscoverer
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

// --- Constants ---
#define DEFAULT_STATS_PORT 9999
#define DEFAULT_VIDEO_BITRATE 4000
#define DEFAULT_AUDIO_BITRATE 128
#define DEFAULT_BUFFER_SIZE_MB 1
#define DEFAULT_BUFFER_SIZE_TIME_MS 20
#define DEFAULT_RECONNECT_TIMEOUT_SEC 10
#define DEFAULT_MAX_RECONNECT_ATTEMPTS 5
#define DEFAULT_WATCHDOG_TIMEOUT_SEC 10
#define DEFAULT_LOW_LATENCY_PRESET "ultrafast"
#define DEFAULT_QUALITY_PRESET "medium"
#define DEFAULT_KEYFRAME_INTERVAL 60

// --- Enum Definitions ---
typedef enum {
    BUFFER_MODE_DEFAULT = 0,
    BUFFER_MODE_LOW_LATENCY = 1,
    BUFFER_MODE_HIGH_QUALITY = 2
} BufferMode;

typedef enum {
    LEAKY_MODE_NONE = 0,
    LEAKY_MODE_UPSTREAM = 1,   // Drop oldest
    LEAKY_MODE_DOWNSTREAM = 2  // Drop newest
} LeakyMode;

// --- Structs ---
typedef struct _QueueLevelData {
    gint current_buffers;        // Current number of buffers in queue
    gint64 current_bytes;        // Current bytes in queue
    gint64 current_time_ns;      // Current queued data duration in ns
    gint max_buffers;            // Maximum number of buffers 
    gint64 max_bytes;            // Maximum bytes 
    gint64 max_time_ns;          // Maximum time in ns
    guint64 overflow_count;      // Number of times queue overflowed
    guint64 underflow_count;     // Number of times queue underflowed
    gdouble percent_full;        // How full the queue is as a percentage
} QueueLevelData;

typedef struct _ProcessingMetrics {
    // General frame metrics
    guint64 frames_processed;
    guint64 frames_dropped;
    guint64 frames_delayed;
    
    // Codec specific metrics
    gdouble avg_qp_value;
    gdouble min_qp_value;
    gdouble max_qp_value;
    gdouble avg_encoding_time_ms;
    
   // A/V sync (updated)
    gdouble audio_video_drift_ms;
    gint64 last_audio_pts;
    gint64 last_video_pts;
    gdouble max_drift_ms;
    
    // Performance metrics
    gdouble cpu_usage_percent;
    guint64 memory_usage_bytes;
    gdouble video_encoding_fps;
    gdouble end_to_end_latency_ms;
    
    // Timestamp data
    gint64 last_input_pts;
    gint64 last_output_pts;
    gint64 timestamp_gap_ns;
    gboolean pts_discontinuity;
} ProcessingMetrics;

typedef struct _NetworkMetrics {
    // Jitter metrics
    gdouble input_jitter_ms;
    gdouble output_jitter_ms;
    
    // Packet metrics
    guint64 total_input_packets;
    guint64 total_output_packets;
    guint64 dropped_packets;
    gdouble avg_packet_size_bytes;
    
    // Connection metrics
    guint64 reconnection_attempts;
    guint64 successful_reconnections;
    gint64 last_reconnection_time;
    gboolean network_stable;
} NetworkMetrics;

typedef struct _StatsData {
    // Original metrics
    gint64 start_time_ns; 
    gint64 input_bytes; 
    gint64 input_packets;
    gint64 last_input_query_time_ns; 
    gdouble current_input_bitrate_bps;
    gint64 output_bytes; 
    gint64 output_packets;
    gint64 last_output_query_time_ns; 
    gdouble current_output_bitrate_bps;
    
    // Extended metrics
    ProcessingMetrics processing;
    NetworkMetrics network;
    
    // Queue level data
    QueueLevelData input_video_queue;
    QueueLevelData input_audio_queue;
    QueueLevelData output_queue;
    QueueLevelData audio_output_queue;
    
    // Historical data (circular buffer style)
    gdouble input_bitrates_history[60];
    gdouble output_bitrates_history[60];
    gint history_index;
    gint num_history_samples;
} StatsData;

typedef struct _RecoveryData {
    gboolean recovery_active;
    gint recovery_attempt_count;
    gint max_recovery_attempts;
    guint watchdog_timer_id;
    gint64 last_activity_time_ns;
    gint reconnect_timeout_sec;
} RecoveryData;

typedef struct _AppData {
    // Core pipeline elements
    GstElement *pipeline; 
    GstElement *video_encoder; 
    GstElement *audio_encoder;
    GstElement *video_queue;
    GstElement *audio_queue;
    GstElement *video_out_queue;
    GstElement *audio_out_queue;
    GstElement *source;
    GstElement *sink;
    
    // Main loops
    GMainLoop *loop; 
    GMainLoop *discovery_loop;
    
    // Network and stats
    guint stats_port; 
    SoupServer *server;
    gchar *input_uri; 
    gchar *output_uri;
    
    // Codec configuration
    gchar *video_codec; 
    gint video_bitrate_kbps;
    gchar *audio_codec; 
    gint audio_bitrate_kbps;
    
    // Discovered input information
    gchar *discovered_input_video_codec; 
    gchar *discovered_input_audio_codec;
    gint discovered_input_video_pid; 
    gint discovered_input_audio_pid;
    gboolean discovery_success; 
    GError *discovery_error;

    gint program_number;        // Program number to select
    gboolean use_program_map;   // Whether to use program mapping or PIDs
    gint manual_video_pid;      // Manual video PID (-1 if auto)
    gint manual_audio_pid;      // Manual audio PID (-1 if auto)
    
    // New buffer and stability options
    BufferMode buffer_mode;
    LeakyMode leaky_mode;
    gint buffer_size_mb;
    gint buffer_size_time_ms;
    gboolean low_latency;
    gchar *encoding_preset;
    gint keyframe_interval;
    gboolean watchdog_enabled;
    gint watchdog_timeout_sec;
    
    // Metrics and recovery data
    StatsData stats; 
    RecoveryData recovery;
    GMutex stats_mutex;
    
    // Additional pipeline and format info
    gint input_width;
    gint input_height;
    gint input_fps_n;
    gint input_fps_d;
    gchar *input_format;
    
    // Flag to indicate whether we should add a clock display for debugging
    gboolean add_clock_overlay;
} AppData;

// --- Global Pointer ---
static GMainLoop *g_main_loop_ptr = NULL;

// --- Function Prototypes ---

// Original function prototypes
static gboolean discover_input_streams(AppData *data, GError **error);
static void on_discoverer_discovered_cb(GstDiscoverer *discoverer, GstDiscovererInfo *info, GError *err, gpointer user_data);
static void on_discoverer_finished_cb(GstDiscoverer *discoverer, gpointer user_data);
static const gchar* get_codec_name_from_caps(GstCaps *caps);
static gint parse_pid_from_stream_id(const gchar* stream_id);
static gboolean build_pipeline_manual(AppData *data, GError **error);
static void demux_pad_added_handler_pid(GstElement *src, GstPad *new_pad, AppData *data);
static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data);
static GstPadProbeReturn input_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static GstPadProbeReturn output_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static void stats_server_callback(SoupServer *server, SoupMessage *msg, const char *path, GHashTable *query, SoupClientContext *client, gpointer user_data);
static void handle_sigint(int sig);
static void calculate_bitrates(AppData *data);
static const gchar* get_decoder_factory(const gchar *codec_name);
static const gchar* get_parser_factory(const gchar *codec_name);

// New function prototypes

static void setup_buffer_properties(AppData *data);
static void update_queue_levels(AppData *data);
static gboolean watchdog_timer_callback(gpointer user_data);
static void start_watchdog_timer(AppData *data);
static void stop_watchdog_timer(AppData *data);
static gboolean attempt_recovery(AppData *data);
static void update_process_metrics(AppData *data);
static void update_network_metrics(AppData *data);
static void setup_av_sync_monitoring(AppData *data);
// static GstPadProbeReturn video_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
// static GstPadProbeReturn audio_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);

// Add these function prototypes
static GstPadProbeReturn video_input_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static GstPadProbeReturn audio_input_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static GstPadProbeReturn video_output_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static GstPadProbeReturn audio_output_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);

static GstPadProbeReturn video_timestamp_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static GstPadProbeReturn audio_timestamp_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static GstPadProbeReturn encoder_quality_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static void update_queue_metrics(GstElement *queue, QueueLevelData *queue_data);
static void init_stats_data(StatsData *stats);
static void init_recovery_data(RecoveryData *recovery, AppData *data);
static JsonNode* create_stats_json(AppData *data);
static JsonNode* create_metrics_json(AppData *data);
static JsonNode* create_buffer_levels_json(AppData *data);
static JsonBuilder* add_queue_data_to_json(JsonBuilder *builder, const char *name, QueueLevelData *queue_data);
static gboolean format_detection_callback(GstElement *typefind, guint probability, GstCaps *caps, gpointer user_data);
static gint find_program_for_stream_id(GstDiscovererStreamInfo *stream_info);
static gint gint_compare(gconstpointer a, gconstpointer b);

// --- Main Function ---
int main(int argc, char *argv[]) {
    AppData data = {0}; 
    GOptionContext *context; 
    GError *error = NULL;
    
    // Extended command line options
    GOptionEntry entries[] = {
        // Original options
        { "input-uri",  'i', 0, G_OPTION_ARG_STRING, &data.input_uri, "Input UDP URI", "URI" },
        { "output-uri", 'o', 0, G_OPTION_ARG_STRING, &data.output_uri,"Output UDP URI", "URI" },
        { "video-codec",'v', 0, G_OPTION_ARG_STRING, &data.video_codec,"Output Video codec (h264, copy)", "CODEC" },
        { "video-bitrate", 'b', 0, G_OPTION_ARG_INT, &data.video_bitrate_kbps, "Video bitrate in kbps", "KBPS" },
        { "audio-codec",'a', 0, G_OPTION_ARG_STRING, &data.audio_codec,"Output Audio codec (aac, copy)", "CODEC" },
        { "audio-bitrate", 'c', 0, G_OPTION_ARG_INT, &data.audio_bitrate_kbps, "Audio bitrate in kbps", "KBPS" },
        { "stats-port", 'p', 0, G_OPTION_ARG_INT, &data.stats_port, "Stats server port", "PORT" },


        // New PID and program selection options
    { "program", 'P', 0, G_OPTION_ARG_INT, &data.program_number, 
      "Program number to select from transport stream", "PROGRAM" },
    { "video-pid", 0, 0, G_OPTION_ARG_INT, &data.manual_video_pid, 
      "Manual video PID selection", "PID" },
    { "audio-pid", 0, 0, G_OPTION_ARG_INT, &data.manual_audio_pid, 
      "Manual audio PID selection", "PID" },
        
        // New buffer control options
        { "buffer-mode", 'm', 0, G_OPTION_ARG_INT, &data.buffer_mode, 
          "Buffer mode (0=default, 1=low-latency, 2=high-quality)", "MODE" },
        { "leaky-mode", 'l', 0, G_OPTION_ARG_INT, &data.leaky_mode, 
          "Leaky queue mode (0=none, 1=upstream/drop-oldest, 2=downstream/drop-newest)", "MODE" },
        { "buffer-size", 's', 0, G_OPTION_ARG_INT, &data.buffer_size_mb, 
          "Buffer size in MB", "MB" },
        { "buffer-time", 't', 0, G_OPTION_ARG_INT, &data.buffer_size_time_ms, 
          "Buffer size in milliseconds", "MS" },
        { "low-latency", 0, 0, G_OPTION_ARG_NONE, &data.low_latency, 
          "Enable low latency mode", NULL },
        { "preset", 0, 0, G_OPTION_ARG_STRING, &data.encoding_preset, 
          "Encoding preset (ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow)", "PRESET" },
        { "keyframe-interval", 'k', 0, G_OPTION_ARG_INT, &data.keyframe_interval, 
          "Keyframe interval in frames", "FRAMES" },
        
        // Recovery options
        { "watchdog", 'w', 0, G_OPTION_ARG_NONE, &data.watchdog_enabled, 
          "Enable watchdog timer for error recovery", NULL },
        { "watchdog-timeout", 0, 0, G_OPTION_ARG_INT, &data.watchdog_timeout_sec, 
          "Watchdog timeout in seconds", "SECONDS" },
        
        // Debug options
        { "add-clock", 0, 0, G_OPTION_ARG_NONE, &data.add_clock_overlay, 
          "Add clock overlay to video (debugging)", NULL },
        
        { NULL }
    };
    
    // Set default values
    data.stats_port = DEFAULT_STATS_PORT; 
    data.video_codec = g_strdup("h264"); 
    data.video_bitrate_kbps = DEFAULT_VIDEO_BITRATE;
    data.audio_codec = g_strdup("aac"); 
    data.audio_bitrate_kbps = DEFAULT_AUDIO_BITRATE;
    data.discovered_input_video_pid = -1; 
    data.discovered_input_audio_pid = -1;
    data.discovery_success = FALSE; 


     // Initialize PID and program selection
data.program_number = -1;       // -1 means auto select
data.use_program_map = TRUE;    // By default use program mapping
data.manual_video_pid = -1;     // -1 means not manually set
data.manual_audio_pid = -1;     // -1 means not manually set
    
    // New default values
    data.buffer_mode = BUFFER_MODE_DEFAULT;
    data.leaky_mode = LEAKY_MODE_NONE;
    data.buffer_size_mb = DEFAULT_BUFFER_SIZE_MB;
    data.buffer_size_time_ms = DEFAULT_BUFFER_SIZE_TIME_MS;
    data.low_latency = FALSE;
    data.encoding_preset = g_strdup(DEFAULT_QUALITY_PRESET);
    data.keyframe_interval = DEFAULT_KEYFRAME_INTERVAL;
    data.watchdog_enabled = FALSE;
    data.watchdog_timeout_sec = DEFAULT_WATCHDOG_TIMEOUT_SEC;
    data.add_clock_overlay = FALSE;
    
    // Initialize mutexes and data structures
    g_mutex_init(&data.stats_mutex);
    init_stats_data(&data.stats);
    init_recovery_data(&data.recovery, &data);
    
    // Parse command line options
    context = g_option_context_new("- Enhanced GStreamer Transcoder");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!gst_init_check(&argc, &argv, &error)) { 
        g_printerr("Failed GST init: %s\n", error->message); 
        g_error_free(error); 
        return EXIT_FAILURE; 
    }
    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse(context, &argc, &argv, &error)) { 
        g_printerr("Option parsing failed: %s\n", error->message); 
        goto cleanup; 
    }
    g_option_context_free(context); 
    context = NULL;
    
    if (!data.input_uri || !data.output_uri) { 
        g_printerr("Error: Input/Output URIs required.\n"); 
        goto cleanup; 
    }

    // Apply low-latency mode settings if enabled
    if (data.low_latency && data.buffer_mode == BUFFER_MODE_DEFAULT) {
        data.buffer_mode = BUFFER_MODE_LOW_LATENCY;
        if (data.encoding_preset) {
            g_free(data.encoding_preset);
        }
        data.encoding_preset = g_strdup(DEFAULT_LOW_LATENCY_PRESET);
        data.buffer_size_time_ms = data.buffer_size_time_ms / 2;  // Half the buffer time
        g_info("Low-latency mode enabled, using ultrafast preset and reduced buffering");
    }

    g_info("Discovering input stream details for: %s", data.input_uri);
    if (!discover_input_streams(&data, &error)) {
        g_printerr("Input stream discovery failed: %s\n", error ? error->message : "Unknown");
        if (data.discovery_error) { 
            g_printerr("  Callback Error: %s\n", data.discovery_error->message); 
        } 
        goto cleanup; 
    }
    
    if (!data.discovery_success || !data.discovered_input_video_codec || 
        !data.discovered_input_audio_codec || data.discovered_input_video_pid < 0 || 
        data.discovered_input_audio_pid < 0) {
         g_printerr("Discovery finished, failed find required video AND audio streams/PIDs.\n");
         if (!error) {
             error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "Stream discovery missing components.");
         }
         goto cleanup; 
    }
    
    g_info("Discovery successful:");
    g_info("  Input Video: %s (PID: %d)", data.discovered_input_video_codec, data.discovered_input_video_pid);
    g_info("  Input Audio: %s (PID: %d)", data.discovered_input_audio_codec, data.discovered_input_audio_pid);
    g_info("  Input Format: %s", data.input_format ? data.input_format : "Unknown");
    if (data.input_width > 0 && data.input_height > 0) {
        g_info("  Input Resolution: %dx%d", data.input_width, data.input_height);
    }
    if (data.input_fps_n > 0 && data.input_fps_d > 0) {
        g_info("  Input Framerate: %d/%d fps", data.input_fps_n, data.input_fps_d);
    }
    
    g_info("Starting Transcoder build:");
    g_info("  Buffer Mode: %d, Leaky Mode: %d", data.buffer_mode, data.leaky_mode);
    g_info("  Buffer Size: %d MB, %d ms", data.buffer_size_mb, data.buffer_size_time_ms);
    g_info("  Output Video: %s @ %d kbps (preset: %s, keyframe interval: %d)",
          data.video_codec, data.video_bitrate_kbps, data.encoding_preset, data.keyframe_interval);
    g_info("  Output Audio: %s @ %d kbps", data.audio_codec, data.audio_bitrate_kbps);
    g_info("  Output URI: %s", data.output_uri);
    g_info("  Stats Port: %u", data.stats_port);
    g_info("  Watchdog: %s (timeout: %d s)", 
          data.watchdog_enabled ? "Enabled" : "Disabled", data.watchdog_timeout_sec);

    if (!build_pipeline_manual(&data, &error)) { 
        g_printerr("Pipeline build failed: %s\n", error? error->message : "Unknown error"); 
        goto cleanup; 
    }
    
    // Set up buffer properties based on configuration
    setup_buffer_properties(&data);

    // Set up A/V sync monitoring
setup_av_sync_monitoring(&data);
g_info("A/V sync monitoring initialized");
    
    // Set up the bus watcher and application loop
    GstBus *bus = gst_element_get_bus(data.pipeline);
    gst_bus_add_watch(bus, bus_callback, &data);
    gst_object_unref(bus);
    
    data.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_ptr = data.loop;
    
    // Set up signal handlers
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    
    // Start stats server
    data.server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "Enhanced Transcoder Stats Server", NULL); 
    if (!data.server) { 
        g_printerr("Failed create SoupServer.\n"); 
        error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create stats server");
        goto cleanup; 
    }
    
    // Add handlers for different endpoints
    soup_server_add_handler(data.server, "/stats", stats_server_callback, &data, NULL);
    soup_server_add_handler(data.server, "/metrics", stats_server_callback, &data, NULL);
    soup_server_add_handler(data.server, "/buffers", stats_server_callback, &data, NULL);
    
    if (!soup_server_listen_local(data.server, data.stats_port, 
                                  SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) { 
        g_printerr("Listen failed %u: %s\n", data.stats_port, error? error->message : "Unknown"); 
        goto cleanup; 
    }
    
    g_info("Stats server listening on:");
    g_info("  http://127.0.0.1:%u/stats    - All stats", data.stats_port);
    g_info("  http://127.0.0.1:%u/metrics  - Processing and network metrics", data.stats_port);
    g_info("  http://127.0.0.1:%u/buffers  - Buffer level information", data.stats_port);
    
    // Initialize stats tracking
    data.stats.start_time_ns = g_get_monotonic_time();
    data.stats.last_input_query_time_ns = data.stats.start_time_ns;
    data.stats.last_output_query_time_ns = data.stats.start_time_ns;
    data.recovery.last_activity_time_ns = data.stats.start_time_ns;
    
    // Start the pipeline
    gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    g_info("Pipeline running...");
    
    // Start watchdog if enabled
    if (data.watchdog_enabled) {
        start_watchdog_timer(&data);
    }
    
    // Run the main loop
    g_main_loop_run(data.loop);

cleanup:
    g_info("Exiting..."); 
    if (error) g_error_free(error); 
    if (context) g_option_context_free(context);
    
    // Stop watchdog timer if active
    stop_watchdog_timer(&data);
    
    // Clean up server
    if (data.server) { 
        soup_server_disconnect(data.server);
        g_object_unref(data.server);
        g_info("Stats server stopped.");
    }
    
    // Clean up pipeline
    if (data.pipeline) { 
        gst_element_set_state(data.pipeline, GST_STATE_NULL);
        g_info("Pipeline stopped.");
        gst_object_unref(data.pipeline);
    }
    
    // Clean up loops
    if (data.loop) { 
        g_main_loop_unref(data.loop);
        g_main_loop_ptr = NULL;
    }
    if (data.discovery_loop) g_main_loop_unref(data.discovery_loop);
    if (data.discovery_error) g_error_free(data.discovery_error);
    
    // Free strings
    g_free(data.input_uri);
    g_free(data.output_uri);
    g_free(data.video_codec);
    g_free(data.audio_codec);
    g_free(data.encoding_preset);
    g_free(data.discovered_input_video_codec);
    g_free(data.discovered_input_audio_codec);
    g_free(data.input_format);
    
    // Clear mutex
    g_mutex_clear(&data.stats_mutex);
    
    return (error || data.discovery_error ? EXIT_FAILURE : EXIT_SUCCESS);
}

static gint gint_compare(gconstpointer a, gconstpointer b) {
    return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}


// --- Initialize Stats Data ---
static void init_stats_data(StatsData *stats) {
    // Zero out the entire structure
    memset(stats, 0, sizeof(StatsData));
    
    // Set any non-zero initial values
    stats->processing.min_qp_value = 100.0;  // Start high for min tracking
    stats->network.network_stable = TRUE;    // Assume stable at start
    
    // Initialize history index
    stats->history_index = 0;
    stats->num_history_samples = 0;

    // Initialize A/V sync-related fields
    stats->processing.last_audio_pts = 0;
    stats->processing.last_video_pts = 0;
    stats->processing.audio_video_drift_ms = 0.0;
    stats->processing.max_drift_ms = 0.0;
}

// --- Initialize Recovery Data ---
static void init_recovery_data(RecoveryData *recovery, AppData *data) {
    recovery->recovery_active = FALSE;
    recovery->recovery_attempt_count = 0;
    recovery->max_recovery_attempts = DEFAULT_MAX_RECONNECT_ATTEMPTS;
    recovery->watchdog_timer_id = 0;
    recovery->last_activity_time_ns = 0;
    recovery->reconnect_timeout_sec = DEFAULT_RECONNECT_TIMEOUT_SEC;
}

// --- Input Stream Discovery Function ---
static gboolean discover_input_streams(AppData *data, GError **error) {
    GstDiscoverer *discoverer;
    guint timeout_id = 0;
    guint discovery_timeout_seconds = 20;
    
    data->discovery_loop = g_main_loop_new(NULL, FALSE);
    discoverer = gst_discoverer_new(10 * GST_SECOND, error);
    if (!discoverer) { 
        g_main_loop_unref(data->discovery_loop);
        data->discovery_loop = NULL;
        return FALSE;
    }
    
    g_signal_connect(discoverer, "discovered", G_CALLBACK(on_discoverer_discovered_cb), data);
    g_signal_connect(discoverer, "finished", G_CALLBACK(on_discoverer_finished_cb), data);
    
    gst_discoverer_start(discoverer);
    if (!gst_discoverer_discover_uri_async(discoverer, data->input_uri)) {
        g_set_error(error, GST_DISCOVERER_ERROR, GST_DISCOVERER_URI_INVALID, 
                   "Failed start discovery for URI");
        gst_discoverer_stop(discoverer);
        g_object_unref(discoverer);
        g_main_loop_unref(data->discovery_loop);
        data->discovery_loop = NULL;
        return FALSE;
    }
    
    timeout_id = g_timeout_add_seconds(discovery_timeout_seconds, 
                                     (GSourceFunc)g_main_loop_quit, data->discovery_loop);
    g_info("Running discovery loop (max %d seconds)...", discovery_timeout_seconds);
    g_main_loop_run(data->discovery_loop);
    g_info("Discovery loop finished.");
    
    if (timeout_id > 0) g_source_remove(timeout_id);
    gst_discoverer_stop(discoverer);
    g_object_unref(discoverer);
    g_main_loop_unref(data->discovery_loop);
    data->discovery_loop = NULL;
    
    return (data->discovery_error == NULL);
}

// --- GstDiscoverer Callbacks ---
static void on_discoverer_discovered_cb(GstDiscoverer *discoverer, GstDiscovererInfo *info, 
                                       GError *err, gpointer user_data) {
    AppData *data = (AppData *)user_data;
    GstDiscovererResult result = gst_discoverer_info_get_result(info);
    
    if (result != GST_DISCOVERER_OK) {
        g_warning("Discovery failed URI %s: %s", 
                  gst_discoverer_info_get_uri(info), 
                  err ? err->message : "Unknown");
        
        if (err && !data->discovery_error) { 
            data->discovery_error = g_error_copy(err); 
        }
        else if (!data->discovery_error) { 
            data->discovery_error = g_error_new(GST_DISCOVERER_ERROR, GST_DISCOVERER_URI_INVALID, 
                                              "Unknown discovery error for URI"); 
        }
        return;
    }
    
    g_info("Discovered URI: %s", gst_discoverer_info_get_uri(info));
    
    // Get the stream list
    GList *streams = gst_discoverer_info_get_stream_list(info);
    gboolean video_found = (data->discovered_input_video_pid >= 0);
    gboolean audio_found = (data->discovered_input_audio_pid >= 0);
    
    // Track programs found
    GHashTable *programs_found = g_hash_table_new(g_direct_hash, g_direct_equal);
    
    // Iterate through the streams
    for (GList *elem = streams; elem; elem = elem->next) {
        GstDiscovererStreamInfo *sinfo = (GstDiscovererStreamInfo *)elem->data;
        GstCaps *caps = gst_discoverer_stream_info_get_caps(sinfo);
        const gchar *stream_id = gst_discoverer_stream_info_get_stream_id(sinfo);
        gint pid = parse_pid_from_stream_id(stream_id);
        gint program = find_program_for_stream_id(sinfo);
        
        // Track this program in our hash table
        if (program >= 0) {
            g_hash_table_insert(programs_found, GINT_TO_POINTER(program), GINT_TO_POINTER(1));
        }
        
        if (caps) {
            const gchar *codec_name = get_codec_name_from_caps(caps);
            g_info("  Stream (ID: %s, PID: %d): Codec: %s, Program: %d", 
                   stream_id ? stream_id : "N/A", 
                   pid, 
                   codec_name ? codec_name : "Unknown",
                   program);
            
            // Check if this stream belongs to the selected program
            gboolean use_this_stream = TRUE;
            
            // Apply program number selection if specified and using program mapping
            if (data->program_number >= 0 && data->use_program_map) {
                use_this_stream = (program == data->program_number);
                if (use_this_stream) {
                    g_info("    -> Matches selected program %d", data->program_number);
                }
            }
            
            // Extract video info if not already found
            if (GST_IS_DISCOVERER_VIDEO_INFO(sinfo) && !video_found && 
                codec_name && pid >= 0 && use_this_stream) {
                
                GstDiscovererVideoInfo *vinfo = GST_DISCOVERER_VIDEO_INFO(sinfo);
                
                // Store codec and PID
                g_free(data->discovered_input_video_codec);
                data->discovered_input_video_codec = g_strdup(codec_name);
                data->discovered_input_video_pid = pid;
                
                // Store video details
                data->input_width = gst_discoverer_video_info_get_width(vinfo);
                data->input_height = gst_discoverer_video_info_get_height(vinfo);
                data->input_fps_n = gst_discoverer_video_info_get_framerate_num(vinfo);
                data->input_fps_d = gst_discoverer_video_info_get_framerate_denom(vinfo);
                
                // Get additional format details from caps
                if (caps) {
                    gchar *caps_str = gst_caps_to_string(caps);
                    if (caps_str) {
                        g_free(data->input_format);
                        data->input_format = g_strdup(caps_str);
                        g_free(caps_str);
                    }
                }
                
                g_info("    -> Selected as INPUT VIDEO stream (Program: %d)", program);
                g_info("    -> Resolution: %dx%d, Framerate: %d/%d fps", 
                       data->input_width, data->input_height, 
                       data->input_fps_n, data->input_fps_d);
                video_found = TRUE;
            }
            // Extract audio info if not already found
            else if (GST_IS_DISCOVERER_AUDIO_INFO(sinfo) && !audio_found && 
                     codec_name && pid >= 0 && use_this_stream) {
                
                GstDiscovererAudioInfo *ainfo = GST_DISCOVERER_AUDIO_INFO(sinfo);
                
                // Store codec and PID
                g_free(data->discovered_input_audio_codec);
                data->discovered_input_audio_codec = g_strdup(codec_name);
                data->discovered_input_audio_pid = pid;
                
                // Get audio details
                guint channels = gst_discoverer_audio_info_get_channels(ainfo);
                guint sample_rate = gst_discoverer_audio_info_get_sample_rate(ainfo);
                
                g_info("    -> Selected as INPUT AUDIO stream (Program: %d)", program);
                g_info("    -> Channels: %u, Sample rate: %u Hz", channels, sample_rate);
                audio_found = TRUE;
            }
            
            gst_caps_unref(caps);
        }
    }
    
    // Now check if manual PIDs were specified
    if (data->manual_video_pid >= 0) {
        g_info("Using manually specified video PID: %d (overriding discovery)", 
              data->manual_video_pid);
        
        // Override the discovered video PID
        data->discovered_input_video_pid = data->manual_video_pid;
        data->use_program_map = FALSE;
        
        // We might need to check if codec is known
        if (data->discovered_input_video_codec == NULL) {
            g_warning("Manual video PID specified but codec info not found. Using h264 as fallback.");
            data->discovered_input_video_codec = g_strdup("h264");
        }
        
        video_found = TRUE;
    }
    
    if (data->manual_audio_pid >= 0) {
        g_info("Using manually specified audio PID: %d (overriding discovery)", 
              data->manual_audio_pid);
        
        // Override the discovered audio PID
        data->discovered_input_audio_pid = data->manual_audio_pid;
        data->use_program_map = FALSE;
        
        // We might need to check if codec is known
        if (data->discovered_input_audio_codec == NULL) {
            g_warning("Manual audio PID specified but codec info not found. Using aac as fallback.");
            data->discovered_input_audio_codec = g_strdup("aac");
        }
        
        audio_found = TRUE;
    }
    
    // Print a list of available programs
    g_info("Available programs in transport stream:");
    if (g_hash_table_size(programs_found) > 0) {
        GList *programs = g_hash_table_get_keys(programs_found);
        programs = g_list_sort(programs, (GCompareFunc)gint_compare);
        
        for (GList *p = programs; p; p = p->next) {
            gint prog_num = GPOINTER_TO_INT(p->data);
            g_info("  Program: %d", prog_num);
        }
        
        g_list_free(programs);
    } else {
        g_info("  No program information found");
    }
    
    g_hash_table_destroy(programs_found);
    gst_discoverer_stream_info_list_free(streams);
    
    if (video_found || audio_found) {
        data->discovery_success = TRUE;
    }
}




static void on_discoverer_finished_cb(GstDiscoverer *discoverer, gpointer user_data) {
    AppData *data = (AppData *)user_data;
    g_info("Discovery process finished signal received.");
    if (data->discovery_loop && g_main_loop_is_running(data->discovery_loop)) {
        g_main_loop_quit(data->discovery_loop);
    } 
}

// --- Helper: get_codec_name_from_caps ---
static const gchar* get_codec_name_from_caps(GstCaps *caps) {
     if (!caps || gst_caps_is_empty(caps)) return NULL; 
     GstStructure *s = gst_caps_get_structure(caps, 0); 
     const gchar* mt = gst_structure_get_name(s);
     
     if (g_str_has_prefix(mt, "video/x-h264")) return "h264"; 
     if (g_str_has_prefix(mt, "video/x-h265")) return "h265"; 
     if (g_str_has_prefix(mt, "video/mpeg")) { 
         gint v = 0; 
         if (gst_structure_get_int(s, "mpegversion", &v) && v == 2) return "mpeg2video"; 
     }
     
     if (g_str_has_prefix(mt, "audio/mpeg")) { 
         gint v = 0, l = 0; 
         gst_structure_get_int(s, "mpegversion", &v); 
         if (v == 1) { 
             gst_structure_get_int(s, "layer", &l); 
             if (l == 2) return "mp2"; 
         } 
     }
     
     if (g_str_has_prefix(mt, "audio/aac")) return "aac"; 
     if (g_str_has_prefix(mt, "audio/x-aac")) return "aac"; 
     if (g_str_has_prefix(mt, "audio/ac3")) return "ac3"; 
     
     return NULL; 
}

// --- Helper: parse_pid_from_stream_id ---
static gint parse_pid_from_stream_id(const gchar* stream_id) {
    if (!stream_id) return -1; 
    gint pid = -1; 
    gchar *endptr = NULL; 
    gchar **parts = g_strsplit(stream_id, "_", -1); 
    const gchar *pid_str_part = stream_id;
    
    if (g_strv_length(parts) >= 3) { 
        pid_str_part = parts[g_strv_length(parts)-1]; 
    } else { 
        g_strfreev(parts); 
        parts = g_strsplit(stream_id, "/", -1); 
        if (g_strv_length(parts) == 2) { 
            pid_str_part = parts[1]; 
        } 
    }
    
    if (pid_str_part) { 
        if (g_str_has_prefix(pid_str_part, "0x") || g_str_has_prefix(pid_str_part, "0X")) { 
            pid = (gint)g_ascii_strtoull(pid_str_part, &endptr, 16); 
        } else { 
            pid = (gint)g_ascii_strtoull(pid_str_part, &endptr, 10); 
            if (endptr == NULL || *endptr != '\0') { 
                pid = (gint)g_ascii_strtoull(pid_str_part, &endptr, 16); 
            } 
        } 
        
        if (endptr == NULL || *endptr != '\0' || pid < 0 || pid > 0x1FFF) { 
            pid = -1; 
        } 
    } 
    
    g_strfreev(parts); 
    g_info("  Parsed PID for '%s' -> %d", stream_id, pid); 
    
    return pid; 
}

// --- Format Detection Callback ---
static gboolean format_detection_callback(GstElement *typefind, guint probability, GstCaps *caps, gpointer user_data) {
    AppData *data = (AppData *)user_data;
    
    if (caps) {
        gchar *caps_str = gst_caps_to_string(caps);
        if (caps_str) {
            g_free(data->input_format);
            data->input_format = g_strdup(caps_str);
            g_info("Detected format: %s (probability: %u%%)", caps_str, probability);
            g_free(caps_str);
        }
    }
    
    return TRUE;
}

// --- Configure Buffer Properties ---
static void setup_buffer_properties(AppData *data) {
    // Calculate buffer sizes in bytes
    guint64 buffer_size_bytes = data->buffer_size_mb * 1024 * 1024;
    
    // Extensive logging for queue setup
    g_info("Setting up Buffer Properties:");
    g_info("  Buffer Mode: %d", data->buffer_mode);
    g_info("  Leaky Mode: %d", data->leaky_mode);
    g_info("  Buffer Size: %d MB (%zu bytes)", data->buffer_size_mb, buffer_size_bytes);
    g_info("  Buffer Time: %d ms", data->buffer_size_time_ms);

    if (!data->video_queue || !data->audio_queue) {
        g_warning("Cannot configure buffer properties, queues not initialized");
        return;
    }

    // Configure video queue based on buffer_mode
    switch (data->buffer_mode) {
        case BUFFER_MODE_LOW_LATENCY:
            g_info("Configuring video queue for low-latency mode");
            g_object_set(data->video_queue,
                        "max-size-buffers", 5,  // Just a few frames
                        "max-size-bytes", buffer_size_bytes / 4,  // 1/4 of total buffer
                        "max-size-time", data->buffer_size_time_ms * GST_MSECOND / 2,  // Half the buffer time
                        NULL);
            break;
            
        case BUFFER_MODE_HIGH_QUALITY:
            g_info("Configuring video queue for high-quality mode");
            g_object_set(data->video_queue,
                        "max-size-buffers", 60,  // More frames to handle hiccups
                        "max-size-bytes", buffer_size_bytes / 2,  // 1/2 of total buffer
                        "max-size-time", data->buffer_size_time_ms * GST_MSECOND * 2,  // Double the buffer time
                        NULL);
            break;
            
        case BUFFER_MODE_DEFAULT:
        default:
            g_info("Configuring video queue for default mode");
            g_object_set(data->video_queue,
                        "max-size-buffers", 30,
                        "max-size-bytes", buffer_size_bytes / 2,
                        "max-size-time", data->buffer_size_time_ms * GST_MSECOND,
                        NULL);
            break;
    }
    
    // Configure audio queue based on buffer_mode
    switch (data->buffer_mode) {
        case BUFFER_MODE_LOW_LATENCY:
            g_info("Configuring audio queue for low-latency mode");
            g_object_set(data->audio_queue,
                        "max-size-buffers", 10,
                        "max-size-bytes", buffer_size_bytes / 8,  // 1/8 of total buffer (audio needs less)
                        "max-size-time", data->buffer_size_time_ms * GST_MSECOND / 2,
                        NULL);
            break;
            
        case BUFFER_MODE_HIGH_QUALITY:
            g_info("Configuring audio queue for high-quality mode");
            g_object_set(data->audio_queue,
                        "max-size-buffers", 100,
                        "max-size-bytes", buffer_size_bytes / 4,
                        "max-size-time", data->buffer_size_time_ms * GST_MSECOND * 2,
                        NULL);
            break;
            
        case BUFFER_MODE_DEFAULT:
        default:
            g_info("Configuring audio queue for default mode");
            g_object_set(data->audio_queue,
                        "max-size-buffers", 50,
                        "max-size-bytes", buffer_size_bytes / 4,
                        "max-size-time", data->buffer_size_time_ms * GST_MSECOND,
                        NULL);
            break;
    }
    
    // Configure leaky behavior if enabled
    if (data->leaky_mode != LEAKY_MODE_NONE) {
        const gchar *leaky_str = (data->leaky_mode == LEAKY_MODE_UPSTREAM) ? "upstream" : "downstream";
        g_info("Setting leaky mode to '%s' for both queues", leaky_str);
        
        g_object_set(data->video_queue, "leaky", data->leaky_mode, NULL);
        g_object_set(data->audio_queue, "leaky", data->leaky_mode, NULL);
    }
    
    // Extensive logging for queue probe setup
    g_info("Setting up Queue Probes:");
    g_info("  Video Queue: %p", (void*)data->video_queue);
    g_info("  Audio Queue: %p", (void*)data->audio_queue);
    g_info("  Video Out Queue: %p", (void*)data->video_out_queue);
    g_info("  Audio Out Queue: %p", (void*)data->audio_out_queue);

    // Set up input queue monitoring probes
    GstPad *video_src_pad = gst_element_get_static_pad(data->video_queue, "src");
    if (video_src_pad) {
        g_info("Adding probe for video input queue");
        gst_pad_add_probe(video_src_pad, 
                         GST_PAD_PROBE_TYPE_BUFFER | 
                         GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | 
                         GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                         video_input_queue_level_probe_cb, data, NULL);
        gst_object_unref(video_src_pad);
    } else {
        g_warning("Failed to get src pad for video input queue");
    }
    
    GstPad *audio_src_pad = gst_element_get_static_pad(data->audio_queue, "src");
    if (audio_src_pad) {
        g_info("Adding probe for audio input queue");
        gst_pad_add_probe(audio_src_pad, 
                         GST_PAD_PROBE_TYPE_BUFFER | 
                         GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | 
                         GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                         audio_input_queue_level_probe_cb, data, NULL);
        gst_object_unref(audio_src_pad);
    } else {
        g_warning("Failed to get src pad for audio input queue");
    }

    // Set up output queue monitoring probes
    if (data->video_out_queue) {
        GstPad *video_out_src_pad = gst_element_get_static_pad(data->video_out_queue, "src");
        if (video_out_src_pad) {
            g_info("Adding probe for video output queue");
            gst_pad_add_probe(video_out_src_pad, 
                             GST_PAD_PROBE_TYPE_BUFFER | 
                             GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | 
                             GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                             video_output_queue_level_probe_cb, data, NULL);
            gst_object_unref(video_out_src_pad);
        } else {
            g_warning("Failed to get src pad for video output queue");
        }
    } else {
        g_warning("Video output queue not initialized");
    }
    
    if (data->audio_out_queue) {
        GstPad *audio_out_src_pad = gst_element_get_static_pad(data->audio_out_queue, "src");
        if (audio_out_src_pad) {
            g_info("Adding probe for audio output queue");
            gst_pad_add_probe(audio_out_src_pad, 
                             GST_PAD_PROBE_TYPE_BUFFER | 
                             GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | 
                             GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                             audio_output_queue_level_probe_cb, data, NULL);
            gst_object_unref(audio_out_src_pad);
        } else {
            g_warning("Failed to get src pad for audio output queue");
        }
    } else {
        g_warning("Audio output queue not initialized");
    }
}

// --- Pad Probe Callbacks ---
static GstPadProbeReturn input_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    GstBuffer *b = gst_pad_probe_info_get_buffer(info);
    
    if (b) {
        gsize s = gst_buffer_get_size(b);
        gint64 current_time = g_get_monotonic_time();
        
        g_mutex_lock(&d->stats_mutex);
        
        // Update activity timestamp for watchdog
        d->recovery.last_activity_time_ns = current_time;
        
        // Basic packet and byte tracking
        d->stats.input_bytes += s;
        d->stats.input_packets++;
        d->stats.network.total_input_packets++;
        
        // Track average packet size
        d->stats.network.avg_packet_size_bytes = 
            (d->stats.network.avg_packet_size_bytes * (d->stats.network.total_input_packets - 1) + s) / 
            d->stats.network.total_input_packets;
        
        // Timestamp tracking for discontinuity detection
        if (GST_BUFFER_PTS_IS_VALID(b)) {
            GstClockTime pts = GST_BUFFER_PTS(b);
            
            if (d->stats.processing.last_input_pts != 0) {
                // Check for discontinuity
                GstClockTimeDiff diff = GST_CLOCK_DIFF(d->stats.processing.last_input_pts, pts);
                
                // If the difference is significantly different from expected frame time
                if (d->input_fps_n > 0 && d->input_fps_d > 0) {
                    GstClockTime expected_diff = (GST_SECOND * d->input_fps_d) / d->input_fps_n;
                    
                    // Allow for some jitter (20% margin)
                    if (diff > expected_diff * 1.2 || diff < expected_diff * 0.8) {
                        d->stats.processing.pts_discontinuity = TRUE;
                        d->stats.processing.timestamp_gap_ns = diff;
                        g_info("PTS discontinuity detected: Expected ~%" GST_TIME_FORMAT ", got %" GST_TIME_FORMAT,
                               GST_TIME_ARGS(expected_diff), GST_TIME_ARGS(diff));
                    } else {
                        d->stats.processing.pts_discontinuity = FALSE;
                    }
                }
            }
            
            d->stats.processing.last_input_pts = pts;
        }
        
        // Debug log to see if bytes are being counted (less frequent now)
        if (d->stats.input_packets % 300 == 0) {
            g_info("PROBE: Input bytes accumulated: %" G_GINT64_FORMAT ", packets: %" G_GINT64_FORMAT,
                  d->stats.input_bytes, d->stats.input_packets);
        }
        
        g_mutex_unlock(&d->stats_mutex);
    }
    
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn output_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    gsize size = 0;
    gint64 current_time = g_get_monotonic_time();
    gboolean have_buffer = FALSE;
    GstBuffer *buffer = NULL;
    
    // Handle different types of probe data
    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer) {
            size = gst_buffer_get_size(buffer);
            have_buffer = TRUE;
        }
    } 
    else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList *list = gst_pad_probe_info_get_buffer_list(info);
        if (list) {
            guint count = gst_buffer_list_length(list);
            for (guint i = 0; i < count; i++) {
                GstBuffer *b = gst_buffer_list_get(list, i);
                if (b) {
                    size += gst_buffer_get_size(b);
                    if (!have_buffer) {
                        buffer = b;  // Use first buffer for timestamp info
                        have_buffer = TRUE;
                    }
                }
            }
        }
    }
    
    if (size > 0) {
        g_mutex_lock(&d->stats_mutex);
        
        // Update activity timestamp for watchdog
        d->recovery.last_activity_time_ns = current_time;
        
        // Basic packet and byte metrics
        d->stats.output_bytes += size;
        d->stats.output_packets++;
        d->stats.network.total_output_packets++;
        
        // Track last output PTS for latency calculation
        if (have_buffer && buffer && GST_BUFFER_PTS_IS_VALID(buffer)) {
            d->stats.processing.last_output_pts = GST_BUFFER_PTS(buffer);
            
            // Calculate end-to-end latency if we have both input and output timestamps
            if (d->stats.processing.last_input_pts > 0) {
                GstClockTimeDiff latency = GST_CLOCK_DIFF(d->stats.processing.last_input_pts, 
                                                        d->stats.processing.last_output_pts);
                
                // Convert to milliseconds and filter out nonsensical values
                if (latency > 0 && latency < 10 * GST_SECOND) {  // Ignore values over 10 seconds
                    d->stats.processing.end_to_end_latency_ms = latency / 1000000.0;
                }
            }
        }
        
        // Log less frequently for normal operation
        if (d->stats.output_packets % 300 == 0) {
            g_info("OUTPUT PROBE: Packets: %" G_GINT64_FORMAT ", current bytes: %zu, total bytes: %" G_GINT64_FORMAT, 
                  d->stats.output_packets, size, d->stats.output_bytes);
            
            if (d->stats.processing.end_to_end_latency_ms > 0) {
                g_info("  Current end-to-end latency: %.2f ms", d->stats.processing.end_to_end_latency_ms);
            }
        }
        
        g_mutex_unlock(&d->stats_mutex);
    }
    
    return GST_PAD_PROBE_OK;
}

// --- Queue Level Monitoring Probes ---
static GstPadProbeReturn video_input_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    
    // Simple print to confirm probe is being called
    //printf("VIDEO INPUT QUEUE PROBE CALLED\n");
    //fflush(stdout);  // Ensure immediate output

    if (!d || !d->video_queue) {
        g_warning("Invalid data in video input queue probe");
        return GST_PAD_PROBE_OK;
    }

    g_mutex_lock(&d->stats_mutex);
    
    // Detailed queue level retrieval
    gint current_buffers = 0;
    gint64 current_bytes = 0;
    gint64 current_time_ns = 0;
    gint max_buffers = 0, max_bytes = 0;
    gint64 max_time_ns = 0;
    
    g_object_get(d->video_queue,
                 "current-level-buffers", &current_buffers,
                 "current-level-bytes", &current_bytes,
                 "current-level-time", &current_time_ns,
                 "max-size-buffers", &max_buffers,
                 "max-size-bytes", &max_bytes,
                 "max-size-time", &max_time_ns,
                 NULL);
    
    // Always log queue details for debugging
   // printf("Video Input Queue Probe:\n");
//printf("  Current Buffers: %d\n", current_buffers);
//printf("  Current Bytes: %ld\n", (long)current_bytes);
//printf("  Current Time: %ld ns\n", (long)current_time_ns);
//printf("  Max Buffers: %d\n", max_buffers);
//printf("  Max Bytes: %ld\n", (long)max_bytes);
//printf("  Max Time: %ld ns\n", (long)max_time_ns);
//fflush(stdout);  // Ensure immediate output
    
    // Update stats unconditionally
    d->stats.input_video_queue.current_buffers = current_buffers;
    d->stats.input_video_queue.current_bytes = current_bytes;
    d->stats.input_video_queue.current_time_ns = current_time_ns;
    d->stats.input_video_queue.max_buffers = max_buffers;
    d->stats.input_video_queue.max_bytes = max_bytes;
    d->stats.input_video_queue.max_time_ns = max_time_ns;
    
    // Calculate percentage full
    gdouble percent_buffers = (max_buffers > 0) ? 
        (100.0 * current_buffers / max_buffers) : 0.0;
    gdouble percent_bytes = (max_bytes > 0) ? 
        (100.0 * current_bytes / max_bytes) : 0.0;
    gdouble percent_time = (max_time_ns > 0) ? 
        (100.0 * current_time_ns / max_time_ns) : 0.0;
    
    d->stats.input_video_queue.percent_full = 
        MAX(percent_buffers, MAX(percent_bytes, percent_time));
    
    // Optional: Track underflow/overflow
    static gint last_buffers = -1;
    if (last_buffers > 0 && current_buffers == 0) {
        d->stats.input_video_queue.underflow_count++;
    }
    
    if (last_buffers == max_buffers && max_buffers > 0) {
        d->stats.input_video_queue.overflow_count++;
    }
    
    last_buffers = current_buffers;
    
    g_mutex_unlock(&d->stats_mutex);
    
    return GST_PAD_PROBE_OK;
}



static GstPadProbeReturn audio_input_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    
    if (d->audio_queue) {
        g_mutex_lock(&d->stats_mutex);
        update_queue_metrics(d->audio_queue, &d->stats.input_audio_queue);
        
        // Add debug logging
        g_info("Audio input queue metrics: buffers=%d, bytes=%" G_GINT64_FORMAT ", time=%" G_GINT64_FORMAT " ns",
              d->stats.input_audio_queue.current_buffers,
              d->stats.input_audio_queue.current_bytes,
              d->stats.input_audio_queue.current_time_ns);
        
        g_mutex_unlock(&d->stats_mutex);
    }
    
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn video_output_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    
    if (d->video_out_queue) {
        g_mutex_lock(&d->stats_mutex);
        update_queue_metrics(d->video_out_queue, &d->stats.output_queue);
        
        // Add debug logging
        g_info("Video output queue metrics: buffers=%d, bytes=%" G_GINT64_FORMAT ", time=%" G_GINT64_FORMAT " ns",
              d->stats.output_queue.current_buffers,
              d->stats.output_queue.current_bytes,
              d->stats.output_queue.current_time_ns);
        
        g_mutex_unlock(&d->stats_mutex);
    }
    
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn audio_output_queue_level_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    
    if (d->audio_out_queue) {
        g_mutex_lock(&d->stats_mutex);
        // Use the dedicated audio output queue structure
        update_queue_metrics(d->audio_out_queue, &d->stats.audio_output_queue);
        
        // Add debug logging
        g_info("Audio output queue metrics: buffers=%d, bytes=%" G_GINT64_FORMAT ", time=%" G_GINT64_FORMAT " ns",
              d->stats.audio_output_queue.current_buffers,
              d->stats.audio_output_queue.current_bytes,
              d->stats.audio_output_queue.current_time_ns);
        
        g_mutex_unlock(&d->stats_mutex);
    }
    
    return GST_PAD_PROBE_OK;
}


static void setup_av_sync_monitoring(AppData *data) {
    g_info("Setting up A/V sync monitoring probes");
    
    // Setup video timestamp probe
    if (data->video_encoder) {
        GstPad *video_src_pad = gst_element_get_static_pad(data->video_encoder, "src");
        if (video_src_pad) {
            g_info("Adding video timestamp probe for A/V sync monitoring");
            gst_pad_add_probe(video_src_pad, 
                             GST_PAD_PROBE_TYPE_BUFFER, 
                             video_timestamp_probe_cb, data, NULL);
            gst_object_unref(video_src_pad);
        } else {
            g_warning("Failed to get src pad from video encoder for A/V sync monitoring");
        }
    } else {
        g_warning("Video encoder not available for A/V sync monitoring");
    }
    
    // Setup audio timestamp probe
    if (data->audio_encoder) {
        GstPad *audio_src_pad = gst_element_get_static_pad(data->audio_encoder, "src");
        if (audio_src_pad) {
            g_info("Adding audio timestamp probe for A/V sync monitoring");
            gst_pad_add_probe(audio_src_pad, 
                             GST_PAD_PROBE_TYPE_BUFFER, 
                             audio_timestamp_probe_cb, data, NULL);
            gst_object_unref(audio_src_pad);
        } else {
            g_warning("Failed to get src pad from audio encoder for A/V sync monitoring");
        }
    } else {
        g_warning("Audio encoder not available for A/V sync monitoring");
    }
    
    g_info("A/V sync monitoring probes setup completed");
}

// --- Video/Audio Timestamp Probes for A/V Sync Monitoring ---
static GstPadProbeReturn video_timestamp_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);
    
    if (buffer && GST_BUFFER_PTS_IS_VALID(buffer)) {
        GstClockTime video_pts = GST_BUFFER_PTS(buffer);
        
        g_mutex_lock(&d->stats_mutex);
        
        // Store the video timestamp
        d->stats.processing.last_video_pts = video_pts;
        
        // If we have both audio and video PTS, calculate the drift
        if (d->stats.processing.last_audio_pts > 0) {
            // Add time base normalization
            GstClockTimeDiff drift;
            
            // Check if the PTS values are in wildly different ranges
            if (video_pts > 3600 * GST_SECOND && d->stats.processing.last_audio_pts < 3600 * GST_SECOND) {
                // Try to normalize by modulo against a reasonable time period (e.g. 1 hour)
                GstClockTime normalized_video = video_pts % (3600 * GST_SECOND);
                drift = GST_CLOCK_DIFF(d->stats.processing.last_audio_pts, normalized_video);
            } else {
                drift = GST_CLOCK_DIFF(d->stats.processing.last_audio_pts, video_pts);
            }
            
            // Sanity check - ignore drift values over 10 seconds
            if (ABS(drift) < 10 * GST_SECOND) {
                // Convert to milliseconds - store as double for consistent handling
                d->stats.processing.audio_video_drift_ms = drift / 1000000.0;
                
                // Track max drift observed (absolute value)
                if (ABS(d->stats.processing.audio_video_drift_ms) > ABS(d->stats.processing.max_drift_ms)) {
                    d->stats.processing.max_drift_ms = d->stats.processing.audio_video_drift_ms;
                }
                
                // No terminal output - silently update the metrics
            } else {
                // Extreme values - don't update the drift metrics
                // Just silently ignore
            }
        }
        
        g_mutex_unlock(&d->stats_mutex);
    }
    
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn audio_timestamp_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);
    
    if (buffer && GST_BUFFER_PTS_IS_VALID(buffer)) {
        GstClockTime audio_pts = GST_BUFFER_PTS(buffer);
        
        g_mutex_lock(&d->stats_mutex);
        
        // Store the audio timestamp
        d->stats.processing.last_audio_pts = audio_pts;
        
        // If we have both audio and video PTS, calculate the drift
        if (d->stats.processing.last_video_pts > 0) {
            GstClockTimeDiff drift;
            
            // Check if the PTS values are in wildly different ranges
            if (d->stats.processing.last_video_pts > 3600 * GST_SECOND && 
                audio_pts < 3600 * GST_SECOND) {
                // Try to normalize by modulo against a reasonable time period (e.g. 1 hour)
                GstClockTime normalized_video = d->stats.processing.last_video_pts % (3600 * GST_SECOND);
                drift = GST_CLOCK_DIFF(audio_pts, normalized_video);
            } else if (audio_pts > 3600 * GST_SECOND && 
                      d->stats.processing.last_video_pts < 3600 * GST_SECOND) {
                // Similar normalization for the opposite case
                GstClockTime normalized_audio = audio_pts % (3600 * GST_SECOND);
                drift = GST_CLOCK_DIFF(normalized_audio, d->stats.processing.last_video_pts);
            } else {
                // Normal case - PTS values in similar range
                drift = GST_CLOCK_DIFF(audio_pts, d->stats.processing.last_video_pts);
            }
            
            // Sanity check - ignore drift values over 10 seconds
            if (ABS(drift) < 10 * GST_SECOND) {
                // Convert to milliseconds - store as double for consistent handling
                d->stats.processing.audio_video_drift_ms = drift / 1000000.0;
                
                // Track max drift observed (absolute value)
                if (ABS(d->stats.processing.audio_video_drift_ms) > ABS(d->stats.processing.max_drift_ms)) {
                    d->stats.processing.max_drift_ms = d->stats.processing.audio_video_drift_ms;
                }
                
                // No terminal output - silently update the metrics
            } else {
                // Extreme values - don't update the drift metrics
                // Just silently ignore
            }
        }
        
        g_mutex_unlock(&d->stats_mutex);
    }
    
    return GST_PAD_PROBE_OK;
}

// --- Encoder Quality Probe for QP Monitoring ---
static GstPadProbeReturn encoder_quality_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    AppData *d = (AppData*)user_data;
    
    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);
        
        if (buffer) {
            // Increment frames processed counter
            g_mutex_lock(&d->stats_mutex);
            d->stats.processing.frames_processed++;
            
            // Update encoding FPS every 30 frames
            if (d->stats.processing.frames_processed % 30 == 0) {
                gint64 now = g_get_monotonic_time();
                gint64 elapsed = now - d->stats.start_time_ns;
                if (elapsed > 0) {
                    d->stats.processing.video_encoding_fps = 
                        (d->stats.processing.frames_processed * 1000000.0) / elapsed;
                }
            }
            
            g_mutex_unlock(&d->stats_mutex);
            
            // Note: For detailed QP value monitoring, we'd need to parse the H.264/H.265 stream
            // This would require additional GStreamer elements or custom processing
        }
    }
    
    return GST_PAD_PROBE_OK;
}

// --- Update Queue Metrics ---
static void update_queue_metrics(GstElement *queue, QueueLevelData *queue_data) {
    gint current_buffers = 0;
    gint64 current_bytes = 0;
    gint64 current_time_ns = 0;
    gint max_buffers = 0;
    gint64 max_bytes = 0;
    gint64 max_time_ns = 0;
    
    // Log queue element name
    g_info("Updating queue metrics for %s", GST_ELEMENT_NAME(queue));
    
    // Get current levels
    g_object_get(queue,
                "current-level-buffers", &current_buffers,
                "current-level-bytes", &current_bytes,
                "current-level-time", &current_time_ns,
                NULL);
    
    // Get max values
    g_object_get(queue,
                "max-size-buffers", &max_buffers,
                "max-size-bytes", &max_bytes,
                "max-size-time", &max_time_ns,
                NULL);
    
    // Log retrieved values
    g_info("  Retrieved current values: buffers=%d, bytes=%" G_GINT64_FORMAT ", time=%" G_GINT64_FORMAT,
          current_buffers, current_bytes, current_time_ns);
    g_info("  Retrieved max values: buffers=%d, bytes=%" G_GINT64_FORMAT ", time=%" G_GINT64_FORMAT,
          max_buffers, max_bytes, max_time_ns);
    
    // Update the data structure
    queue_data->current_buffers = current_buffers;
    queue_data->current_bytes = current_bytes;
    queue_data->current_time_ns = current_time_ns;
    queue_data->max_buffers = max_buffers;
    queue_data->max_bytes = max_bytes;
    queue_data->max_time_ns = max_time_ns;
    
    // Calculate fullness percentage based on the most constrained dimension
    gdouble percent_buffers = (max_buffers > 0) ? 
        (100.0 * current_buffers / max_buffers) : 0.0;
    
    gdouble percent_bytes = (max_bytes > 0) ? 
        (100.0 * current_bytes / max_bytes) : 0.0;
    
    gdouble percent_time = (max_time_ns > 0) ? 
        (100.0 * current_time_ns / max_time_ns) : 0.0;
    
    // Use the highest percentage
    queue_data->percent_full = MAX(percent_buffers, MAX(percent_bytes, percent_time));
    
    g_info("  Calculated percent full: %.2f%%", queue_data->percent_full);
    
    // Detect underflow/overflow conditions
    static gint last_buffers = -1;
    
    if (last_buffers > 0 && current_buffers == 0) {
        queue_data->underflow_count++;
        g_info("  Queue underflow detected, count: %" G_GUINT64_FORMAT, queue_data->underflow_count);
    } else if (last_buffers == max_buffers && max_buffers > 0) {
        queue_data->overflow_count++;
        g_info("  Queue overflow detected, count: %" G_GUINT64_FORMAT, queue_data->overflow_count);
    }
    
    last_buffers = current_buffers;
}

// --- Watchdog and Recovery Functions ---
static gboolean watchdog_timer_callback(gpointer user_data) {
    AppData *data = (AppData *)user_data;
    gint64 current_time = g_get_monotonic_time();
    
    // Check if there's been activity within the timeout period
    gint64 time_since_last_activity = current_time - data->recovery.last_activity_time_ns;
    gint64 timeout_ns = data->watchdog_timeout_sec * G_USEC_PER_SEC;
    
    g_info("Watchdog check: %.2f seconds since last activity (timeout: %d seconds)",
           time_since_last_activity / 1000000.0, data->watchdog_timeout_sec);
    
    if (time_since_last_activity > timeout_ns) {
        g_warning("Watchdog timeout detected! No activity for %.2f seconds",
                 time_since_last_activity / 1000000.0);
        
        // Attempt recovery if we're not already in recovery mode
        if (!data->recovery.recovery_active) {
            return attempt_recovery(data);
        } else {
            g_warning("Recovery already in progress, waiting...");
        }
    }
    
    // Watchdog remains active
    return G_SOURCE_CONTINUE;
}

static void start_watchdog_timer(AppData *data) {
    if (data->recovery.watchdog_timer_id == 0) {
        g_info("Starting watchdog timer with %d second timeout", data->watchdog_timeout_sec);
        data->recovery.watchdog_timer_id = g_timeout_add_seconds(
            data->watchdog_timeout_sec / 2,  // Check twice per timeout period
            watchdog_timer_callback, data);
    }
}

static void stop_watchdog_timer(AppData *data) {
    if (data->recovery.watchdog_timer_id > 0) {
        g_info("Stopping watchdog timer");
        g_source_remove(data->recovery.watchdog_timer_id);
        data->recovery.watchdog_timer_id = 0;
    }
}

static gboolean attempt_recovery(AppData *data) {
    // Increment recovery attempt counter
    data->recovery.recovery_attempt_count++;
    data->recovery.recovery_active = TRUE;
    
    g_warning("Attempting pipeline recovery (attempt %d of %d)",
             data->recovery.recovery_attempt_count, data->recovery.max_recovery_attempts);
    
    // Update network metrics
    g_mutex_lock(&data->stats_mutex);
    data->stats.network.reconnection_attempts++;
    data->stats.network.network_stable = FALSE;
    g_mutex_unlock(&data->stats_mutex);
    
    // If we've exceeded max attempts, shut down
    if (data->recovery.recovery_attempt_count > data->recovery.max_recovery_attempts) {
        g_critical("Maximum recovery attempts (%d) exceeded, shutting down",
                  data->recovery.max_recovery_attempts);
        
        // Signal main loop to quit
        if (data->loop && g_main_loop_is_running(data->loop)) {
            g_main_loop_quit(data->loop);
        }
        
        return G_SOURCE_REMOVE;  // Remove watchdog timer
    }
    
    // Step 1: Try to reset the pipeline state
    GstState current_state, pending_state;
    GstStateChangeReturn ret = gst_element_get_state(data->pipeline, 
                                                   &current_state, &pending_state, 0);
    
    g_info("Current pipeline state: %s, pending: %s, get_state returned: %d",
          gst_element_state_get_name(current_state),
          gst_element_state_get_name(pending_state),
          ret);
    
    // Try NULL -> READY -> PAUSED -> PLAYING transition
    g_info("Setting pipeline to NULL state");
    gst_element_set_state(data->pipeline, GST_STATE_NULL);
    
    g_info("Waiting 1 second before state change");
    g_usleep(1 * G_USEC_PER_SEC);  // Give it time to settle
    
    g_info("Setting pipeline to READY state");
    gst_element_set_state(data->pipeline, GST_STATE_READY);
    g_usleep(500000);  // 0.5 sec
    
    g_info("Setting pipeline to PAUSED state");
    gst_element_set_state(data->pipeline, GST_STATE_PAUSED);
    g_usleep(500000);  // 0.5 sec
    
    g_info("Setting pipeline to PLAYING state");
    gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
    
    // Reset activity timestamp
    data->recovery.last_activity_time_ns = g_get_monotonic_time();
    
    // Update status
    g_mutex_lock(&data->stats_mutex);
    data->stats.network.last_reconnection_time = g_get_real_time() / 1000000;  // Unix timestamp
    data->recovery.recovery_active = FALSE;
    g_mutex_unlock(&data->stats_mutex);
    
    g_info("Recovery attempt completed, resuming normal operation");
    
    return G_SOURCE_CONTINUE;  // Keep watchdog running
}

// --- Update Process and Network Metrics ---
static void update_process_metrics(AppData *data) {
    struct rusage usage;
    
    // Get resource usage for this process
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        g_mutex_lock(&data->stats_mutex);
        
        // Get current time for calculating interval
        gint64 current_time = g_get_monotonic_time();
        
        // Calculate total CPU time in microseconds
        gint64 current_cpu_time = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000LL +
                                  (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
        
        // Check if we have a previous measurement
        static gint64 last_cpu_time = 0;
        static gint64 last_wall_time = 0;
        static gboolean first_call = TRUE;
        
        if (!first_call) {
            // Calculate deltas
            gint64 cpu_time_delta = current_cpu_time - last_cpu_time;
            gint64 wall_time_delta = current_time - last_wall_time;
            
            if (wall_time_delta > 0) {
                // Calculate CPU usage percentage
                gdouble cpu_usage = (gdouble)cpu_time_delta / (gdouble)wall_time_delta * 100.0;
                
                // Get number of CPU cores
                gint num_cores = g_get_num_processors();
                if (num_cores <= 0) {
                    num_cores = 1;  // Fallback
                }
                
                // Normalize to 0-100% scale (divide by number of cores)
                cpu_usage = cpu_usage / num_cores;
                
                // Clamp to reasonable bounds
                if (cpu_usage < 0.0) cpu_usage = 0.0;
                if (cpu_usage > 100.0) cpu_usage = 100.0;
                
                data->stats.processing.cpu_usage_percent = cpu_usage;
            }
        } else {
            // First call - initialize but don't calculate
            data->stats.processing.cpu_usage_percent = 0.0;
            first_call = FALSE;
        }
        
        // Update last measurements for next time
        last_cpu_time = current_cpu_time;
        last_wall_time = current_time;
        
        // Memory usage in bytes (maxrss is in kilobytes)
        data->stats.processing.memory_usage_bytes = usage.ru_maxrss * 1024;
        
        g_mutex_unlock(&data->stats_mutex);
    }
}

static void update_network_metrics(AppData *data) {
    // This would typically involve analyzing jitter, packet loss, etc.
    // For now, we'll use a simple approach based on what we're already tracking
    
    g_mutex_lock(&data->stats_mutex);
    
    // Update jitter metrics (simplified approach)
    // In a real implementation, you would track timestamps and calculate true jitter
    if (data->stats.processing.pts_discontinuity) {
        data->stats.network.input_jitter_ms = ABS(data->stats.processing.timestamp_gap_ns) / 1000000.0;
    }
    
    // Estimate dropped packets if we know the expected packet rate
    // This is very implementation-specific and depends on transport protocol
    
    g_mutex_unlock(&data->stats_mutex);
}

// --- Update Queue Levels ---
static void update_queue_levels(AppData *data) {
    g_info("Updating all queue levels");
    
    // Update input video queue
    if (data->video_queue) {
        g_mutex_lock(&data->stats_mutex);
        update_queue_metrics(data->video_queue, &data->stats.input_video_queue);
        g_mutex_unlock(&data->stats_mutex);
    }
    
    // Update input audio queue
    if (data->audio_queue) {
        g_mutex_lock(&data->stats_mutex);
        update_queue_metrics(data->audio_queue, &data->stats.input_audio_queue);
        g_mutex_unlock(&data->stats_mutex);
    }
    
    // Update output video queue
    if (data->video_out_queue) {
        g_mutex_lock(&data->stats_mutex);
        update_queue_metrics(data->video_out_queue, &data->stats.output_queue);
        g_mutex_unlock(&data->stats_mutex);
    }
    
    // Update output audio queue with dedicated structure
    if (data->audio_out_queue) {
        g_mutex_lock(&data->stats_mutex);
        update_queue_metrics(data->audio_out_queue, &data->stats.audio_output_queue);
        g_mutex_unlock(&data->stats_mutex);
    }
}

// --- Build Pipeline (Manual Linking, uses DISCOVERED Input Codecs) ---

static gboolean build_pipeline_manual(AppData *data, GError **error) {
    GstElement *source = NULL, *demux = NULL, *mux = NULL, *sink = NULL;
    GstElement *video_queue = NULL, *video_parser = NULL, *video_decode = NULL, *video_convert = NULL;
    GstElement *video_encode = NULL, *video_out_parser = NULL, *video_scale = NULL, *video_rate = NULL;
    GstElement *video_overlay = NULL;
    GstElement *audio_queue = NULL, *audio_parser = NULL, *audio_decode = NULL, *audio_convert = NULL;
    GstElement *audio_resample = NULL, *audio_encode = NULL;
    GstElement *video_out_queue = NULL, *audio_out_queue = NULL;
    GstElement *mux_out_queue = NULL; // Output queue after muxer
    GstElement *watchdog = NULL; // Watchdog element
    gboolean link_ok; GstPad *sinkpad = NULL, *srcpad = NULL; gulong probe_id;
    gchar *output_host = NULL; gchar *port_str = NULL; guint output_port = 0;

    const gchar *vid_dec_f = get_decoder_factory(data->discovered_input_video_codec);
    const gchar *aud_dec_f = get_decoder_factory(data->discovered_input_audio_codec);
    const gchar *vid_par_f = get_parser_factory(data->discovered_input_video_codec);
    const gchar *aud_par_f = get_parser_factory(data->discovered_input_audio_codec);
    
    if (!vid_dec_f || !aud_dec_f) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, 
                    "Unsupported discovered input codec ('%s'/'%s'). Cannot find decoder.", 
                    data->discovered_input_video_codec, data->discovered_input_audio_codec);
        return FALSE;
    }
    
    g_info("Using video decoder: %s, audio decoder: %s", vid_dec_f, aud_dec_f);
    if(vid_par_f) 
        g_info("Using video parser: %s", vid_par_f); 
    else 
        g_info("No video parser needed/found for %s", data->discovered_input_video_codec);
    
    if(aud_par_f) 
        g_info("Using audio parser: %s", aud_par_f); 
    else 
        g_info("No audio parser needed/found for %s", data->discovered_input_audio_codec);

    // Create pipeline
    data->pipeline = gst_pipeline_new("transcoder-pipeline");
    if (!data->pipeline) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to create pipeline");
        goto build_fail_cleanup;
    }

    // Create elements
    source = gst_element_factory_make("udpsrc", "udp-source");
    demux = gst_element_factory_make("tsdemux", "ts-demuxer");
    video_queue = gst_element_factory_make("queue", "video-queue");
    if (vid_par_f) 
        video_parser = gst_element_factory_make(vid_par_f, "video-parser");
    video_decode = gst_element_factory_make(vid_dec_f, "video-decoder");
    video_convert = gst_element_factory_make("videoconvert", "video-converter");
    video_scale = gst_element_factory_make("videoscale", "video-scaler");
    video_rate = gst_element_factory_make("videorate", "video-rate");
    
    // Add optional clock overlay for debugging
    if (data->add_clock_overlay) {
        video_overlay = gst_element_factory_make("clockoverlay", "clock-overlay");
    }
    
    audio_queue = gst_element_factory_make("queue", "audio-queue");
    if (aud_par_f) 
        audio_parser = gst_element_factory_make(aud_par_f, "audio-parser");
    audio_decode = gst_element_factory_make(aud_dec_f, "audio-decoder");
    audio_convert = gst_element_factory_make("audioconvert", "audio-converter");
    audio_resample = gst_element_factory_make("audioresample", "audio-resampler");
    
    // Create muxer with improved latency settings
    mux = gst_element_factory_make("mpegtsmux", "ts-muxer");
    if (mux) {
        // Configure muxer with appropriate latency settings
        guint64 total_bitrate = (data->video_bitrate_kbps + data->audio_bitrate_kbps) * 1000;
        
        g_object_set(mux, 
                    "bitrate", total_bitrate,
                    "alignment", 7,
                    "latency", 2000 * GST_MSECOND,
                    NULL);
    }
    
    // Create new output queues using regular queue elements
    video_out_queue = gst_element_factory_make("queue", "video-out-queue");
    audio_out_queue = gst_element_factory_make("queue", "audio-out-queue");
    
    // Create new queue between muxer and sink
    mux_out_queue = gst_element_factory_make("queue", "mux-out-queue");
    
    // Create watchdog element
    watchdog = gst_element_factory_make("watchdog", "stream-watchdog");
    if (watchdog) {
        // Configure watchdog timeout using the same value from AppData
        gint watchdog_timeout_ms = data->watchdog_timeout_sec * 1000;
        g_object_set(watchdog, "timeout", watchdog_timeout_ms, NULL);
        
        // Enable the application's watchdog recovery mechanism
        data->watchdog_enabled = TRUE;
        
        g_info("Configured GStreamer watchdog with timeout: %d ms", watchdog_timeout_ms);
    }
    
    data->video_out_queue = video_out_queue;
    data->audio_out_queue = audio_out_queue;
    
    sink = gst_element_factory_make("udpsink", "udp-sink");
    
    // Store queue elements for later configuration
    data->video_queue = video_queue;
    data->audio_queue = audio_queue;
    data->source = source;
    data->sink = sink;

    // Check element creation
    if (!source || !demux || !mux || !sink || !video_queue || (vid_par_f && !video_parser) || 
        !video_decode || !video_convert || !video_scale || !video_rate || !audio_queue || 
        (aud_par_f && !audio_parser) || !audio_decode || !audio_convert || !audio_resample ||
        (data->add_clock_overlay && !video_overlay) || !video_out_queue || !audio_out_queue || 
        !mux_out_queue || !watchdog) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to create elements");
        goto build_fail_cleanup;
    }

    // Configure source and probe
    g_object_set(source, "uri", data->input_uri, NULL);
    g_object_set(source, "buffer-size", 4194304, NULL);    // 4MB buffer, increased
    g_object_set(source, "timeout", 2000000000, NULL);     // 2s timeout in ns, increased
    
    srcpad = gst_element_get_static_pad(source, "src");
    if (!srcpad) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "No src pad on udpsrc");
        goto build_fail_cleanup;
    }
    probe_id = gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER, input_probe_cb, data, NULL);
    if (probe_id == 0) 
        g_warning("Failed to add input probe");
    gst_object_unref(srcpad);
    srcpad = NULL;

    // Parse output URI and configure sink
    const gchar *uri_body = g_strstr_len(data->output_uri, -1, "udp://");
    if (!uri_body) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Invalid output URI scheme (expected udp://)");
        goto build_fail_cleanup;
    }
    
    uri_body += strlen("udp://");
    gchar *colon = g_strrstr(uri_body, ":");
    if (!colon || colon == uri_body) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Invalid output URI format (expected host:port)");
        goto build_fail_cleanup;
    }
    
    output_host = g_strndup(uri_body, colon - uri_body);
    port_str = g_strdup(colon + 1);
    output_port = atoi(port_str);
    g_free(port_str);
    port_str = NULL;
    
    if (output_port == 0) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Invalid output port");
        g_free(output_host);
        output_host = NULL;
        goto build_fail_cleanup;
    }
    
    // Enhanced UDP sink configuration
    g_object_set(sink, "host", output_host, "port", output_port, "sync", FALSE, NULL);
    g_object_set(sink, "buffer-size", 4194304, NULL);      // 4MB buffer size, increased
    g_object_set(sink, "max-lateness", -1, NULL);          // Allow late buffers
    g_object_set(sink, "qos", TRUE, NULL);                // Disable QoS
    g_object_set(sink, "async", TRUE, NULL);              // Disable async behavior for better latency
    
    g_free(output_host);
    output_host = NULL;
    
    // Add output probe
    sinkpad = gst_element_get_static_pad(sink, "sink");
    if (!sinkpad) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "No sink pad on udpsink");
        goto build_fail_cleanup;
    }
    
    // Try a more comprehensive probe type to catch all data
    probe_id = gst_pad_add_probe(sinkpad, 
                                GST_PAD_PROBE_TYPE_BUFFER | 
                                GST_PAD_PROBE_TYPE_BUFFER_LIST |
                                GST_PAD_PROBE_TYPE_PUSH, 
                                output_probe_cb, data, NULL);
    if (probe_id == 0) 
        g_warning("Failed to add output probe");
    gst_object_unref(sinkpad);
    sinkpad = NULL;

    // Create video encoder
    if (g_strcmp0(data->video_codec, "copy") == 0) {
        video_encode = gst_element_factory_make("identity", "video-encoder");
    } else if (g_strcmp0(data->video_codec, "h264") == 0) {
        video_encode = gst_element_factory_make("x264enc", "video-encoder");
        if (video_encode) {
            // Configure for CBR live streaming
            g_object_set(video_encode,
                "bitrate", (guint)data->video_bitrate_kbps,
                "pass", 0,           // String value "cbr" for constant bitrate
                "tune", 0x00000004,      // zerolatency
                "byte-stream", TRUE,     // Proper stream format
                "vbv-buf-capacity", 50, // 8000ms buffer size
                "rc-lookahead", 10,      // Small lookahead for rate stability
                "mb-tree", FALSE,        // Disable mb-tree which can cause fluctuations
                "threads", 4,            // Limit threads for more consistent encoding
                NULL);

            // Disable B-frames for live streaming (This block was already correct for no B-frames)
            g_object_set(video_encode,
                        "b-adapt", FALSE,
                        "bframes", 0,
                        NULL);

            // Set encoding preset based on configuration
            if (data->encoding_preset) {
                g_info("Setting x264 preset to '%s' with CBR mode", data->encoding_preset);

                // Map preset string to x264enc speed-preset value
                gint speed_preset = 6;  // Default "medium"
                if (g_strcmp0(data->encoding_preset, "ultrafast") == 0) speed_preset = 1;
                else if (g_strcmp0(data->encoding_preset, "superfast") == 0) speed_preset = 2;
                else if (g_strcmp0(data->encoding_preset, "veryfast") == 0) speed_preset = 3;
                else if (g_strcmp0(data->encoding_preset, "faster") == 0) speed_preset = 4;
                else if (g_strcmp0(data->encoding_preset, "fast") == 0) speed_preset = 5;
                else if (g_strcmp0(data->encoding_preset, "medium") == 0) speed_preset = 6;
                else if (g_strcmp0(data->encoding_preset, "slow") == 0) speed_preset = 7;
                else if (g_strcmp0(data->encoding_preset, "slower") == 0) speed_preset = 8;
                else if (g_strcmp0(data->encoding_preset, "veryslow") == 0) speed_preset = 9;

                g_object_set(video_encode, "speed-preset", speed_preset, NULL);
            } else {
                // For live, default to a faster preset if none specified
                g_object_set(video_encode, "speed-preset", 7, NULL); // veryfast
                g_info("No preset specified, defaulting to 'veryfast' for live CBR encoding");
            }

            g_info("Configured x264enc for live CBR: bitrate=%d kbps, vbv-buf-capacity=%d kbit, vbv-max-bitrate=%d kbit",
                  data->video_bitrate_kbps,
                  data->video_bitrate_kbps, // Reflects the new vbv-buf-capacity setting
                  data->video_bitrate_kbps  // Reflects the new vbv-max-bitrate setting
                  );

            // Add parser for the encoded output
            video_out_parser = gst_element_factory_make("h264parse", "video-out-parser");
            if (!video_out_parser) {
                g_warning("Failed to create h264parse for output");
            } else {
                // Important for downstream elements to understand stream properties,
                // especially for clients joining mid-stream. Sends SPS/PPS with I-frames.
                g_object_set(video_out_parser, "config-interval", -1, NULL);
            }

            // Add encoder quality probe
            GstPad *enc_src_pad = gst_element_get_static_pad(video_encode, "src");
            if (enc_src_pad) {
                gst_pad_add_probe(enc_src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                encoder_quality_probe_cb, data, NULL);
                gst_object_unref(enc_src_pad);
            }
        }
    }
    
    if (!video_encode) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to create video encoder");
        goto build_fail_cleanup;
    }
    data->video_encoder = video_encode;

    // Create audio encoder
    if (g_strcmp0(data->audio_codec, "copy") == 0) {
        audio_encode = gst_element_factory_make("identity", "audio-encoder");
    } else if (g_strcmp0(data->audio_codec, "aac") == 0) {
        audio_encode = gst_element_factory_make("avenc_aac", "audio-encoder");
        if (audio_encode) 
            g_object_set(audio_encode, "bitrate", data->audio_bitrate_kbps * 1000, NULL);
    }
    
    if (!audio_encode) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to create audio encoder");
        goto build_fail_cleanup;
    }
    data->audio_encoder = audio_encode;

    // Configure the new output queues - use regular queue not queue2
    guint64 buffer_size_bytes = data->buffer_size_mb * 1024 * 1024;
    
    // Configure output queues with larger buffers
    g_object_set(video_out_queue,
               "max-size-buffers", 60,         // Doubled
               "max-size-bytes", buffer_size_bytes / 2,  // Increased
               "max-size-time", 3000 * GST_MSECOND,      // Increased to 3 seconds
               NULL);
               
    // Set leaky property for regular queue
    if (data->leaky_mode != LEAKY_MODE_NONE) {
        gint leaky_val = (data->leaky_mode == LEAKY_MODE_UPSTREAM) ? 1 : 2;
        g_object_set(video_out_queue, "leaky", leaky_val, NULL);
    }
    
    g_object_set(audio_out_queue,
               "max-size-buffers", 60,          // Doubled
               "max-size-bytes", buffer_size_bytes / 4,
               "max-size-time", 3000 * GST_MSECOND,  // Increased to 3 seconds
               NULL);
               
    // Set leaky property for regular queue
    if (data->leaky_mode != LEAKY_MODE_NONE) {
        gint leaky_val = (data->leaky_mode == LEAKY_MODE_UPSTREAM) ? 1 : 2;
        g_object_set(audio_out_queue, "leaky", leaky_val, NULL);
    }
    
    // Configure the muxer output queue
    g_object_set(mux_out_queue,
               "max-size-buffers", 100,         // Larger buffer for muxed output
               "max-size-bytes", buffer_size_bytes,  // Full buffer size
               "max-size-time", 5000 * GST_MSECOND,  // 5 seconds buffer
               NULL);
    
    // Set leaky property if needed (usually downstream for output)
    if (data->leaky_mode != LEAKY_MODE_NONE) {
        gint leaky_val = (data->leaky_mode == LEAKY_MODE_UPSTREAM) ? 1 : 2;
        g_object_set(mux_out_queue, "leaky", leaky_val, NULL);
        g_info("Setting leaky mode %d on muxer output queue", leaky_val);
    }
    
    g_info("Added muxer output queue with %d buffers, %zu bytes, %d ms", 
           100, buffer_size_bytes, 5000);

    // Add elements to pipeline
    gst_bin_add_many(GST_BIN(data->pipeline), source, demux, mux, mux_out_queue, watchdog, sink, 
                     video_queue, video_decode, video_convert, video_scale, video_rate, 
                     video_encode, audio_queue, audio_decode, audio_convert, 
                     audio_resample, audio_encode, video_out_queue, audio_out_queue, NULL);
                     
    if (video_parser) 
        gst_bin_add(GST_BIN(data->pipeline), video_parser);
    if (audio_parser) 
        gst_bin_add(GST_BIN(data->pipeline), audio_parser);
    if (video_out_parser) 
        gst_bin_add(GST_BIN(data->pipeline), video_out_parser);
    if (video_overlay)
        gst_bin_add(GST_BIN(data->pipeline), video_overlay);

    // Link source to demux
    if (!gst_element_link(source, demux)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link source to demux");
        goto build_fail_cleanup;
    }

    // Link video elements
    GstElement* current_video = video_queue;
    
    if (video_parser) {
        if (!gst_element_link(current_video, video_parser)) {
            g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link video queue to parser");
            goto build_fail_cleanup;
        }
        current_video = video_parser;
    }
    
    if (!gst_element_link(current_video, video_decode)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to video decoder");
        goto build_fail_cleanup;
    }
    current_video = video_decode;
    
    if (!gst_element_link(current_video, video_convert)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to video converter");
        goto build_fail_cleanup;
    }
    current_video = video_convert;
    
    if (!gst_element_link(current_video, video_scale)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to video scaler");
        goto build_fail_cleanup;
    }
    current_video = video_scale;
    
    if (!gst_element_link(current_video, video_rate)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to video rate");
        goto build_fail_cleanup;
    }
    current_video = video_rate;
    
    // Insert clock overlay if requested
    if (video_overlay) {
        if (!gst_element_link(current_video, video_overlay)) {
            g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to clock overlay");
            goto build_fail_cleanup;
        }
        current_video = video_overlay;
    }
    
    if (!gst_element_link(current_video, video_encode)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to video encoder");
        goto build_fail_cleanup;
    }
    current_video = video_encode;
    
    // Add parser if needed
    if (video_out_parser) {
        if (!gst_element_link(current_video, video_out_parser)) {
            g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to video output parser");
            goto build_fail_cleanup;
        }
        current_video = video_out_parser;
    }
    
    // Link to the new output queue
    if (!gst_element_link(current_video, video_out_queue)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to video output queue");
        goto build_fail_cleanup;
    }

    // Link audio elements
    GstElement* current_audio = audio_queue;
    if (audio_parser) {
        if (!gst_element_link(current_audio, audio_parser)) {
            g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link audio queue to parser");
            goto build_fail_cleanup;
        }
        current_audio = audio_parser;
    }
    
    if (!gst_element_link(current_audio, audio_decode)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to audio decoder");
        goto build_fail_cleanup;
    }
    current_audio = audio_decode;
    
    if (!gst_element_link(current_audio, audio_convert)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to audio converter");
        goto build_fail_cleanup;
    }
    current_audio = audio_convert;
    
    if (!gst_element_link(current_audio, audio_resample)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to audio resampler");
        goto build_fail_cleanup;
    }
    current_audio = audio_resample;
    
    if (!gst_element_link(current_audio, audio_encode)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to audio encoder");
        goto build_fail_cleanup;
    }
    
    // Link to the new audio output queue
    if (!gst_element_link(audio_encode, audio_out_queue)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link to audio output queue");
        goto build_fail_cleanup;
    }

    // Link mux to mux_out_queue
    if (!gst_element_link(mux, mux_out_queue)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link muxer to muxer output queue");
        goto build_fail_cleanup;
    }
    
    // Link mux_out_queue to watchdog
    if (!gst_element_link(mux_out_queue, watchdog)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link muxer output queue to watchdog");
        goto build_fail_cleanup;
    }
    
    // Link watchdog to sink
    if (!gst_element_link(watchdog, sink)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Failed to link watchdog to sink");
        goto build_fail_cleanup;
    }

    // Connect demuxer pad-added signal
    g_signal_connect(demux, "pad-added", G_CALLBACK(demux_pad_added_handler_pid), data);
    
    g_info("Pipeline built with additional output queues to improve latency handling");
    g_info("Added muxer output queue between mpegtsmux and udpsink for smoother transmission");
    g_info("Added GStreamer watchdog with %d ms timeout - will trigger pipeline restart on stall", 
           data->watchdog_timeout_sec * 1000);
    
    return TRUE;

build_fail_cleanup:
    if (sinkpad) gst_object_unref(sinkpad);
    if (srcpad) gst_object_unref(srcpad);
    if (output_host) g_free(output_host);
    if (port_str) g_free(port_str);
    
    if (data->pipeline) {
        gst_object_unref(data->pipeline);
        data->pipeline = NULL;
    }
    return FALSE;
}

// --- Pad Added Handler (Uses Discovered PIDs) ---
static void demux_pad_added_handler_pid(GstElement *src, GstPad *new_pad, AppData *data) {
    gchar *pad_name = gst_pad_get_name(new_pad);
    g_info("Pad added handler called for demuxer pad: %s", pad_name);
    
    gint pad_pid = parse_pid_from_stream_id(pad_name);
    if (pad_pid < 0) {
        g_warning("Could not parse PID from pad name: %s", pad_name);
        goto cleanup_pad;
    }
    
    GstPad *target_sink_pad = NULL;
    gboolean is_video = (pad_pid == data->discovered_input_video_pid);
    gboolean is_audio = (pad_pid == data->discovered_input_audio_pid);
    GstElement* target_queue = NULL;
    
    if (is_video) {
        target_queue = gst_bin_get_by_name(GST_BIN(data->pipeline), "video-queue");
        if (target_queue)
            g_info("Pad matches video PID (%d)", data->discovered_input_video_pid);
    }
    else if (is_audio) {
        target_queue = gst_bin_get_by_name(GST_BIN(data->pipeline), "audio-queue");
        if (target_queue)
            g_info("Pad matches audio PID (%d)", data->discovered_input_audio_pid);
    }
    else {
        g_info("Ignoring pad %s PID %d (not matched)", pad_name, pad_pid);
        goto cleanup_pad;
    }
    
    if (!target_queue) {
        g_warning("Queue not found for PID %d", pad_pid);
        goto cleanup_pad;
    }
    
    target_sink_pad = gst_element_get_static_pad(target_queue, "sink");
    gst_object_unref(target_queue);
    
    if (!target_sink_pad) {
        g_warning("No sink pad for queue PID %d", pad_pid);
        goto cleanup_pad;
    }
    
    if (gst_pad_is_linked(target_sink_pad)) {
        g_info("Queue sink %s already linked.", GST_PAD_NAME(target_sink_pad));
        gst_object_unref(target_sink_pad);
        goto cleanup_pad;
    }
    
    GstPadLinkReturn ret = gst_pad_link(new_pad, target_sink_pad);
    if (GST_PAD_LINK_FAILED(ret)) {
        g_warning("Link demux->queue failed (PID %d, ret=%d)", pad_pid, ret);
        gst_object_unref(target_sink_pad);
        goto cleanup_pad;
    }
    
    g_info("Linked demux pad %s to queue sink %s.", pad_name, GST_PAD_NAME(target_sink_pad));
    gst_object_unref(target_sink_pad);
    
    // Now link output queue to the muxer
    GstElement *muxer = gst_bin_get_by_name(GST_BIN(data->pipeline), "ts-muxer");
    if (!muxer) {
        g_warning("No muxer found");
        goto cleanup_pad;
    }
    
    // Get the appropriate output queue based on stream type
    const gchar *queue_name = is_video ? "video-out-queue" : "audio-out-queue";
    GstElement *output_queue = gst_bin_get_by_name(GST_BIN(data->pipeline), queue_name);
    
    if (!output_queue) {
        g_warning("No output queue found for %s", is_video ? "video" : "audio");
        gst_object_unref(muxer);
        goto cleanup_pad;
    }
    
    // Get source pad from output queue
    GstPad *queue_src_pad = gst_element_get_static_pad(output_queue, "src");
    if (!queue_src_pad) {
        g_warning("No src pad from output queue for %s", is_video ? "video" : "audio");
        gst_object_unref(output_queue);
        gst_object_unref(muxer);
        goto cleanup_pad;
    }
    
    // Try to get a sink pad from muxer using request_pad
    GstPad *mux_sink = NULL;
    
    // Try several different template names that might work with mpegtsmux
    const gchar* templates[] = {"sink_%u", "sink_%d", "video", "audio", "sink", NULL};
    for (int i = 0; templates[i] != NULL && !mux_sink; i++) {
        g_info("Trying pad template: %s", templates[i]);
        mux_sink = gst_element_request_pad_simple(muxer, templates[i]);
    }
    
    if (!mux_sink) {
        // Try with explicit template instead
        GstPadTemplate *templ = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(muxer), "sink_%u");
        if (templ) {
            mux_sink = gst_element_request_pad(muxer, templ, NULL, NULL);
            g_info("Requested pad using explicit template: %s", mux_sink ? "SUCCESS" : "FAILED");
        }
    }
    
    if (!mux_sink) {
        g_warning("Failed to request any mux sink pad for %s", is_video ? "video" : "audio");
        gst_object_unref(queue_src_pad);
        gst_object_unref(output_queue);
        gst_object_unref(muxer);
        goto cleanup_pad;
    }
    
    // Link output_queue to muxer
    if (gst_pad_is_linked(mux_sink)) {
        g_warning("Mux sink pad already linked!");
        gst_object_unref(mux_sink);
        gst_object_unref(queue_src_pad);
        gst_object_unref(output_queue);
        gst_object_unref(muxer);
        goto cleanup_pad;
    }
    
    ret = gst_pad_link(queue_src_pad, mux_sink);
    g_info("Output queue to muxer link for %s: %s", 
           is_video ? "video" : "audio", 
           GST_PAD_LINK_SUCCESSFUL(ret) ? "SUCCESS" : "FAILED");
    
    if (!GST_PAD_LINK_SUCCESSFUL(ret)) {
        g_warning("Pad link failed with code %d", ret);
        
        // Debug caps negotiation
        GstCaps *src_caps = gst_pad_get_current_caps(queue_src_pad);
        if (src_caps) {
            gchar *caps_str = gst_caps_to_string(src_caps);
            g_info("Source caps: %s", caps_str);
            g_free(caps_str);
            gst_caps_unref(src_caps);
        } else {
            g_warning("No caps on source pad");
        }
        
        GstCaps *sink_caps = gst_pad_get_current_caps(mux_sink);
        if (sink_caps) {
            gchar *caps_str = gst_caps_to_string(sink_caps);
            g_info("Sink caps: %s", caps_str);
            g_free(caps_str);
            gst_caps_unref(sink_caps);
        } else {
            // Try to get pad template caps
            GstCaps *template_caps = gst_pad_get_pad_template_caps(mux_sink);
            if (template_caps) {
                gchar *caps_str = gst_caps_to_string(template_caps);
                g_info("Sink template caps: %s", caps_str);
                g_free(caps_str);
                gst_caps_unref(template_caps);
            } else {
                g_warning("No caps on sink pad or template");
            }
        }
    } else {
        g_info("Successfully linked %s output queue to muxer", is_video ? "video" : "audio");
    }
    
    gst_object_unref(mux_sink);
    gst_object_unref(queue_src_pad);
    gst_object_unref(output_queue);
    gst_object_unref(muxer);
    
cleanup_pad:
    g_free(pad_name);
}



static gint find_program_for_stream_id(GstDiscovererStreamInfo *stream_info) {
    // Check if we can get program information directly from this stream
    const GstStructure *s = gst_discoverer_stream_info_get_misc(stream_info);
    if (s) {
        gint program_number = -1;
        if (gst_structure_has_field(s, "program-number")) {
            gst_structure_get_int(s, "program-number", &program_number);
            return program_number;
        }
    }
    
    // No need to check parent/container as the GStreamer API doesn't 
    // provide a clean way to navigate up the stream hierarchy
    
    return -1;  // Program information not found
}


// --- GStreamer Bus Callback (Enhanced for debugging & recovery) ---
static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data) { 
    AppData *d = (AppData*)data; 
    switch(GST_MESSAGE_TYPE(message)) { 
        case GST_MESSAGE_ERROR: { 
            gchar *dbg; 
            GError *e; 
            gst_message_parse_error(message, &e, &dbg); 
            g_error("ERR from %s: %s (%s)", GST_OBJECT_NAME(message->src), e->message, dbg ? dbg : ""); 
            
            // Check if this is a watchdog timeout error
            gboolean is_watchdog_error = (g_strcmp0(GST_OBJECT_NAME(message->src), "stream-watchdog") == 0);
            if (is_watchdog_error) {
                g_warning("Watchdog timeout detected! Triggering recovery");
            }
            
            g_error_free(e); 
            g_free(dbg); 
            
            // Update network metrics
            g_mutex_lock(&d->stats_mutex);
            d->stats.network.network_stable = FALSE;
            g_mutex_unlock(&d->stats_mutex);
            
            // For watchdog errors, always attempt recovery regardless of other settings
            if (is_watchdog_error || (d->watchdog_enabled && !d->recovery.recovery_active)) {
                g_warning("Triggering recovery due to %s", 
                         is_watchdog_error ? "watchdog timeout" : "error");
                attempt_recovery(d);
            } else {
                g_main_loop_quit(d->loop);
            }
            break; 
        } 
        case GST_MESSAGE_WARNING: { 
            gchar *dbg; 
            GError *e; 
            gst_message_parse_warning(message, &e, &dbg); 
            g_warning("WARN from %s: %s (%s)", GST_OBJECT_NAME(message->src), e->message, dbg ? dbg : ""); 
            g_error_free(e); 
            g_free(dbg);
            
            // Update activity timestamp for watchdog
            d->recovery.last_activity_time_ns = g_get_monotonic_time();
            break; 
        } 
        case GST_MESSAGE_INFO: { 
            gchar *dbg; 
            GError *e; 
            gst_message_parse_info(message, &e, &dbg); 
            g_info("INFO from %s: %s (%s)", GST_OBJECT_NAME(message->src), e->message, dbg ? dbg : ""); 
            g_error_free(e); 
            g_free(dbg);
            
            // Update activity timestamp for watchdog
            d->recovery.last_activity_time_ns = g_get_monotonic_time();
            break; 
        }
        case GST_MESSAGE_EOS: 
            g_info("EOS."); 
            g_main_loop_quit(d->loop); 
            break; 
        case GST_MESSAGE_STATE_CHANGED: 
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(d->pipeline)) {
                GstState o, n, p; 
                gst_message_parse_state_changed(message, &o, &n, &p); 
                g_info("Pipeline state: %s->%s", 
                      gst_element_state_get_name(o), 
                      gst_element_state_get_name(n));
                      
                // If we've successfully transitioned to PLAYING, mark network as stable
                if (n == GST_STATE_PLAYING && o != GST_STATE_PLAYING) {
                    g_mutex_lock(&d->stats_mutex);
                    d->stats.network.network_stable = TRUE;
                    if (d->recovery.recovery_active) {
                        d->stats.network.successful_reconnections++;
                        d->recovery.recovery_active = FALSE;
                    }
                    g_mutex_unlock(&d->stats_mutex);
                }
            }
            // Log state changes for muxer
            else if (g_str_has_prefix(GST_OBJECT_NAME(message->src), "ts-muxer")) {
                GstState o, n, p; 
                gst_message_parse_state_changed(message, &o, &n, &p); 
                g_info("Muxer state: %s->%s", 
                      gst_element_state_get_name(o), 
                      gst_element_state_get_name(n));
            }
            break;
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(message);
            if (s && gst_structure_has_name(s, "GstMultiQueueLevels")) {
                g_info("MultiQueue levels update from %s", GST_OBJECT_NAME(message->src));
            }
            break;
        }
        case GST_MESSAGE_STREAM_STATUS: {
            GstStreamStatusType type;
            GstElement *owner;
            gst_message_parse_stream_status(message, &type, &owner);
            g_info("Stream status change: %d for %s", type, GST_OBJECT_NAME(owner));
            
            // Update activity timestamp for watchdog
            d->recovery.last_activity_time_ns = g_get_monotonic_time();
            break;
        }
        case GST_MESSAGE_BUFFERING: {
            gint percent;
            gst_message_parse_buffering(message, &percent);
            g_info("Buffering: %d%%", percent);
            
            // Update activity timestamp for watchdog
            d->recovery.last_activity_time_ns = g_get_monotonic_time();
            break;
        }
        case GST_MESSAGE_QOS: {
            // Process QoS messages for performance tracking
            gboolean live;
            guint64 running_time, stream_time, timestamp, duration;
            gint64 jitter;
            gdouble proportion;
            gint quality;
            
            gst_message_parse_qos(message, &live, &running_time, &stream_time, &timestamp, &duration);
            gst_message_parse_qos_values(message, &jitter, &proportion, &quality);
            
            g_info("QoS: jitter: %" G_GINT64_FORMAT " ns, proportion: %f, quality: %d", 
                  jitter, proportion, quality);
            
            // Update jitter metric
            g_mutex_lock(&d->stats_mutex);
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(d->sink)) {
                d->stats.network.output_jitter_ms = ABS(jitter) / 1000000.0;
            }
            g_mutex_unlock(&d->stats_mutex);
            
            // Update activity timestamp for watchdog
            d->recovery.last_activity_time_ns = g_get_monotonic_time();
            break;
        }
        default: 
            // Uncomment for verbose debugging
            // g_info("Unhandled message type: %s from %s", 
            //      GST_MESSAGE_TYPE_NAME(message), GST_OBJECT_NAME(message->src));
            break; 
    } 
    return TRUE; 
}

static void calculate_bitrates(AppData *d) {
    gint64 now = g_get_monotonic_time();
    gint64 input_bytes, input_packets;
    gint64 output_bytes, output_packets;
    gint64 time_diff_input, time_diff_output;
    
    g_mutex_lock(&d->stats_mutex);
    
    // Get current values
    input_bytes = d->stats.input_bytes;
    input_packets = d->stats.input_packets;
    output_bytes = d->stats.output_bytes;
    output_packets = d->stats.output_packets;
    
    // Calculate time differences in microseconds
    time_diff_input = now - d->stats.last_input_query_time_ns;
    time_diff_output = now - d->stats.last_output_query_time_ns;
    
    // Calculate input bitrate (convert microseconds to seconds)
    if (time_diff_input > 500000) { // At least 0.5 seconds
        if (input_bytes > 0) {
            // Calculate bits per second: bytes * 8 / seconds
            d->stats.current_input_bitrate_bps = 
                (double)input_bytes * 8.0 * 1000000.0 / (double)time_diff_input;
            
            // Store in history buffer for trend analysis
            d->stats.input_bitrates_history[d->stats.history_index] = 
                d->stats.current_input_bitrate_bps / 1000000.0;  // Store in Mbps
            
            g_info("Calculated input bitrate: %.2f Mbps from %" G_GINT64_FORMAT 
                   " bytes over %.2f seconds", 
                   d->stats.current_input_bitrate_bps / 1000000.0,
                   input_bytes, 
                   time_diff_input / 1000000.0);
            
            // Reset counters
            d->stats.input_bytes = 0;
            d->stats.last_input_query_time_ns = now;
        }
    }
    
    // Calculate output bitrate
    if (time_diff_output > 500000) { // At least 0.5 seconds
        if (output_bytes > 0) {
            // Calculate bits per second: bytes * 8 / seconds
            d->stats.current_output_bitrate_bps = 
                (double)output_bytes * 8.0 * 1000000.0 / (double)time_diff_output;
            
            // Store in history buffer
            d->stats.output_bitrates_history[d->stats.history_index] = 
                d->stats.current_output_bitrate_bps / 1000000.0;  // Store in Mbps
            
            // Increment history index
            d->stats.history_index = (d->stats.history_index + 1) % 60;
            if (d->stats.num_history_samples < 60)
                d->stats.num_history_samples++;
            
            g_info("Calculated output bitrate: %.2f Mbps from %" G_GINT64_FORMAT 
                   " bytes over %.2f seconds", 
                   d->stats.current_output_bitrate_bps / 1000000.0,
                   output_bytes, 
                   time_diff_output / 1000000.0);
            
            // Reset counters
            d->stats.output_bytes = 0;
            d->stats.last_output_query_time_ns = now;
        }
    }

    // Force update queue levels for all queues
    if (d->video_queue) {
        gint current_buffers = 0;
        gint64 current_bytes = 0;
        gint64 current_time_ns = 0;
        gint max_buffers = 0, max_bytes = 0;
        gint64 max_time_ns = 0;
        
        g_object_get(d->video_queue,
                     "current-level-buffers", &current_buffers,
                     "current-level-bytes", &current_bytes,
                     "current-level-time", &current_time_ns,
                     "max-size-buffers", &max_buffers,
                     "max-size-bytes", &max_bytes,
                     "max-size-time", &max_time_ns,
                     NULL);
        
        // Update video input queue stats
        d->stats.input_video_queue.current_buffers = current_buffers;
        d->stats.input_video_queue.current_bytes = current_bytes;
        d->stats.input_video_queue.current_time_ns = current_time_ns;
        d->stats.input_video_queue.max_buffers = max_buffers;
        d->stats.input_video_queue.max_bytes = max_bytes;
        d->stats.input_video_queue.max_time_ns = max_time_ns;
        
        // Calculate percentage full
        gdouble percent_buffers = (max_buffers > 0) ? 
            (100.0 * current_buffers / max_buffers) : 0.0;
        gdouble percent_bytes = (max_bytes > 0) ? 
            (100.0 * current_bytes / max_bytes) : 0.0;
        gdouble percent_time = (max_time_ns > 0) ? 
            (100.0 * current_time_ns / max_time_ns) : 0.0;
        
        d->stats.input_video_queue.percent_full = 
            MAX(percent_buffers, MAX(percent_bytes, percent_time));
    }

    // Repeat similar logic for audio_queue
    if (d->audio_queue) {
        gint current_buffers = 0;
        gint64 current_bytes = 0;
        gint64 current_time_ns = 0;
        gint max_buffers = 0, max_bytes = 0;
        gint64 max_time_ns = 0;
        
        g_object_get(d->audio_queue,
                     "current-level-buffers", &current_buffers,
                     "current-level-bytes", &current_bytes,
                     "current-level-time", &current_time_ns,
                     "max-size-buffers", &max_buffers,
                     "max-size-bytes", &max_bytes,
                     "max-size-time", &max_time_ns,
                     NULL);
        
        // Update audio input queue stats
        d->stats.input_audio_queue.current_buffers = current_buffers;
        d->stats.input_audio_queue.current_bytes = current_bytes;
        d->stats.input_audio_queue.current_time_ns = current_time_ns;
        d->stats.input_audio_queue.max_buffers = max_buffers;
        d->stats.input_audio_queue.max_bytes = max_bytes;
        d->stats.input_audio_queue.max_time_ns = max_time_ns;
        
        // Calculate percentage full
        gdouble percent_buffers = (max_buffers > 0) ? 
            (100.0 * current_buffers / max_buffers) : 0.0;
        gdouble percent_bytes = (max_bytes > 0) ? 
            (100.0 * current_bytes / max_bytes) : 0.0;
        gdouble percent_time = (max_time_ns > 0) ? 
            (100.0 * current_time_ns / max_time_ns) : 0.0;
        
        d->stats.input_audio_queue.percent_full = 
            MAX(percent_buffers, MAX(percent_bytes, percent_time));
    }

    // Check and update output queue if exists
    if (d->video_out_queue) {
        gint current_buffers = 0;
        gint64 current_bytes = 0;
        gint64 current_time_ns = 0;
        gint max_buffers = 0, max_bytes = 0;
        gint64 max_time_ns = 0;
        
        g_object_get(d->video_out_queue,
                     "current-level-buffers", &current_buffers,
                     "current-level-bytes", &current_bytes,
                     "current-level-time", &current_time_ns,
                     "max-size-buffers", &max_buffers,
                     "max-size-bytes", &max_bytes,
                     "max-size-time", &max_time_ns,
                     NULL);
        
        // Update output queue stats
        d->stats.output_queue.current_buffers = current_buffers;
        d->stats.output_queue.current_bytes = current_bytes;
        d->stats.output_queue.current_time_ns = current_time_ns;
        d->stats.output_queue.max_buffers = max_buffers;
        d->stats.output_queue.max_bytes = max_bytes;
        d->stats.output_queue.max_time_ns = max_time_ns;
        
        // Calculate percentage full
        gdouble percent_buffers = (max_buffers > 0) ? 
            (100.0 * current_buffers / max_buffers) : 0.0;
        gdouble percent_bytes = (max_bytes > 0) ? 
            (100.0 * current_bytes / max_bytes) : 0.0;
        gdouble percent_time = (max_time_ns > 0) ? 
            (100.0 * current_time_ns / max_time_ns) : 0.0;
        
        d->stats.output_queue.percent_full = 
            MAX(percent_buffers, MAX(percent_bytes, percent_time));
    }
    
    // Check and update audio output queue if exists
    if (d->audio_out_queue) {
        gint current_buffers = 0;
        gint64 current_bytes = 0;
        gint64 current_time_ns = 0;
        gint max_buffers = 0, max_bytes = 0;
        gint64 max_time_ns = 0;
        
        g_object_get(d->audio_out_queue,
                     "current-level-buffers", &current_buffers,
                     "current-level-bytes", &current_bytes,
                     "current-level-time", &current_time_ns,
                     "max-size-buffers", &max_buffers,
                     "max-size-bytes", &max_bytes,
                     "max-size-time", &max_time_ns,
                     NULL);
        
        // Update audio output queue stats
        d->stats.audio_output_queue.current_buffers = current_buffers;
        d->stats.audio_output_queue.current_bytes = current_bytes;
        d->stats.audio_output_queue.current_time_ns = current_time_ns;
        d->stats.audio_output_queue.max_buffers = max_buffers;
        d->stats.audio_output_queue.max_bytes = max_bytes;
        d->stats.audio_output_queue.max_time_ns = max_time_ns;
        
        // Calculate percentage full
        gdouble percent_buffers = (max_buffers > 0) ? 
            (100.0 * current_buffers / max_buffers) : 0.0;
        gdouble percent_bytes = (max_bytes > 0) ? 
            (100.0 * current_bytes / max_bytes) : 0.0;
        gdouble percent_time = (max_time_ns > 0) ? 
            (100.0 * current_time_ns / max_time_ns) : 0.0;
        
        d->stats.audio_output_queue.percent_full = 
            MAX(percent_buffers, MAX(percent_bytes, percent_time));
    }


    g_mutex_unlock(&d->stats_mutex);
    
    // Update process and network metrics less frequently
    static gint update_counter = 0;
    if (update_counter++ % 5 == 0) {
        update_process_metrics(d);
        update_network_metrics(d);
        update_queue_levels(d);
    }
}


// --- Factory Helper Functions ---
static const gchar* get_decoder_factory(const gchar *c) {
    if (!c) return NULL;
    
    if (g_strcmp0(c, "h264") == 0) return "avdec_h264";
    if (g_strcmp0(c, "mpeg2video") == 0) return "avdec_mpeg2video";
    if (g_strcmp0(c, "h265") == 0) return "avdec_h265";
    if (g_strcmp0(c, "aac") == 0) return "avdec_aac";
    if (g_strcmp0(c, "mp2") == 0 || g_strcmp0(c, "mpga") == 0) return "avdec_mp2float";
    if (g_strcmp0(c, "ac3") == 0) return "avdec_ac3";
    
    g_warning("Unknown decoder factory for codec %s", c);
    return NULL;
}

static const gchar* get_parser_factory(const gchar *c) {
    if (!c) return NULL;
    
    if (g_strcmp0(c, "h264") == 0) return "h264parse";
    if (g_strcmp0(c, "mpeg2video") == 0) return "mpegvideoparse";
    if (g_strcmp0(c, "h265") == 0) return "h265parse";
    if (g_strcmp0(c, "aac") == 0) return "aacparse";
    if (g_strcmp0(c, "mp2") == 0 || g_strcmp0(c, "mpga") == 0) return "mpegaudioparse";
    
    // No need for a parser for some formats
    g_info("No parser defined for codec %s, this may be normal", c);
    return NULL;
}

// --- Signal Handler ---
static void handle_sigint(int sig) {
    g_warning("Caught signal %d, quitting...", sig);
    if (g_main_loop_ptr && g_main_loop_is_running(g_main_loop_ptr)) {
        g_main_loop_quit(g_main_loop_ptr);
    }
}

// --- Add Json Builder Helper ---
static JsonBuilder* add_queue_data_to_json(JsonBuilder *builder, const char *name, QueueLevelData *queue_data) {
    json_builder_set_member_name(builder, name);
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "percent_full");
    json_builder_add_double_value(builder, queue_data->percent_full);
    
    json_builder_set_member_name(builder, "current_buffers");
    json_builder_add_int_value(builder, queue_data->current_buffers);
    
    json_builder_set_member_name(builder, "current_bytes");
    json_builder_add_int_value(builder, queue_data->current_bytes);
    
    json_builder_set_member_name(builder, "current_time_ms");
    json_builder_add_double_value(builder, queue_data->current_time_ns / 1000000.0);
    
    json_builder_set_member_name(builder, "overflow_count");
    json_builder_add_int_value(builder, queue_data->overflow_count);
    
    json_builder_set_member_name(builder, "underflow_count");
    json_builder_add_int_value(builder, queue_data->underflow_count);
    
    json_builder_end_object(builder);
    
    return builder;
}

// --- Create JSON responses for different API endpoints ---
static JsonNode* create_stats_json(AppData *data) {
    JsonBuilder *builder = json_builder_new();
    JsonNode *root = NULL;
    gint64 uptime_seconds;
    GstState current_state, pending_state;
    
    // Start building JSON
    json_builder_begin_object(builder);
    
    // Add timestamp
    json_builder_set_member_name(builder, "timestamp_unix");
    json_builder_add_int_value(builder, g_get_real_time() / 1000000);
    
    // Add uptime
    g_mutex_lock(&data->stats_mutex);
    uptime_seconds = (g_get_monotonic_time() - data->stats.start_time_ns) / 1000000;
    g_mutex_unlock(&data->stats_mutex);
    
    json_builder_set_member_name(builder, "uptime_seconds");
    json_builder_add_int_value(builder, uptime_seconds);
    
    // Add basic stats
    g_mutex_lock(&data->stats_mutex);
    
    // Input/output stats
    json_builder_set_member_name(builder, "input_bitrate_mbps");
    json_builder_add_double_value(builder, data->stats.current_input_bitrate_bps / 1000000.0);
    
    json_builder_set_member_name(builder, "output_bitrate_mbps");
    json_builder_add_double_value(builder, data->stats.current_output_bitrate_bps / 1000000.0);
    
    json_builder_set_member_name(builder, "input_packets_total");
    json_builder_add_int_value(builder, data->stats.network.total_input_packets);
    
    json_builder_set_member_name(builder, "output_packets_total");
    json_builder_add_int_value(builder, data->stats.network.total_output_packets);
    
    // Add bitrate history
    json_builder_set_member_name(builder, "bitrate_history");
    json_builder_begin_object(builder);
    
    // Input bitrate history
    json_builder_set_member_name(builder, "input_mbps");
    json_builder_begin_array(builder);
    for (int i = 0; i < data->stats.num_history_samples; i++) {
        int idx = (data->stats.history_index - i - 1 + 60) % 60;  // Go backwards from current
        json_builder_add_double_value(builder, data->stats.input_bitrates_history[idx]);
    }
    json_builder_end_array(builder);
    
    // Output bitrate history
    json_builder_set_member_name(builder, "output_mbps");
    json_builder_begin_array(builder);
    for (int i = 0; i < data->stats.num_history_samples; i++) {
        int idx = (data->stats.history_index - i - 1 + 60) % 60;  // Go backwards from current
        json_builder_add_double_value(builder, data->stats.output_bitrates_history[idx]);
    }
    json_builder_end_array(builder);
    
    json_builder_end_object(builder);  // End bitrate_history
    
    g_mutex_unlock(&data->stats_mutex);
    
    // Add pipeline state
    gst_element_get_state(data->pipeline, &current_state, &pending_state, 0);
    json_builder_set_member_name(builder, "pipeline_state");
    json_builder_add_string_value(builder, gst_element_state_get_name(current_state));
    
    // Add codec info
    json_builder_set_member_name(builder, "video_codec");
    json_builder_add_string_value(builder, data->video_codec ? data->video_codec : "unknown");
    
    json_builder_set_member_name(builder, "video_bitrate_kbps");
    json_builder_add_int_value(builder, data->video_bitrate_kbps);
    
    json_builder_set_member_name(builder, "audio_codec");
    json_builder_add_string_value(builder, data->audio_codec ? data->audio_codec : "unknown");
    
    json_builder_set_member_name(builder, "audio_bitrate_kbps");
    json_builder_add_int_value(builder, data->audio_bitrate_kbps);
    
    // Add discovered input info
    json_builder_set_member_name(builder, "input_info");
    json_builder_begin_object(builder);

     // Update create_stats_json function - add to input_info section
json_builder_set_member_name(builder, "video_pid");
json_builder_add_int_value(builder, data->discovered_input_video_pid);

json_builder_set_member_name(builder, "audio_pid");
json_builder_add_int_value(builder, data->discovered_input_audio_pid);

json_builder_set_member_name(builder, "program_number");
json_builder_add_int_value(builder, data->program_number >= 0 ? data->program_number : -1);

json_builder_set_member_name(builder, "manually_selected_pids");
json_builder_add_boolean_value(builder, 
                              (data->manual_video_pid >= 0 || data->manual_audio_pid >= 0));
    
    json_builder_set_member_name(builder, "video_codec");
    json_builder_add_string_value(builder, data->discovered_input_video_codec ? 
                                 data->discovered_input_video_codec : "unknown");
    
    json_builder_set_member_name(builder, "audio_codec");
    json_builder_add_string_value(builder, data->discovered_input_audio_codec ? 
                                 data->discovered_input_audio_codec : "unknown");
    
    json_builder_set_member_name(builder, "resolution");
    if (data->input_width > 0 && data->input_height > 0) {
        gchar *res = g_strdup_printf("%dx%d", data->input_width, data->input_height);
        json_builder_add_string_value(builder, res);
        g_free(res);
    } else {
        json_builder_add_string_value(builder, "unknown");
    }
    
    json_builder_set_member_name(builder, "framerate");
    if (data->input_fps_n > 0 && data->input_fps_d > 0) {
        gdouble fps = (gdouble)data->input_fps_n / (gdouble)data->input_fps_d;
        json_builder_add_double_value(builder, fps);
    } else {
        json_builder_add_double_value(builder, 0.0);
    }
    
    json_builder_end_object(builder);  // End input_info
    
    // Add buffer section
    json_builder_set_member_name(builder, "buffer_info");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "mode");
    switch (data->buffer_mode) {
        case BUFFER_MODE_LOW_LATENCY:
            json_builder_add_string_value(builder, "low-latency");
            break;
        case BUFFER_MODE_HIGH_QUALITY:
            json_builder_add_string_value(builder, "high-quality");
            break;
        default:
            json_builder_add_string_value(builder, "default");
            break;
    }
    
    json_builder_set_member_name(builder, "leaky_mode");
    switch (data->leaky_mode) {
        case LEAKY_MODE_UPSTREAM:
            json_builder_add_string_value(builder, "upstream");
            break;
        case LEAKY_MODE_DOWNSTREAM:
            json_builder_add_string_value(builder, "downstream");
            break;
        default:
            json_builder_add_string_value(builder, "none");
            break;
    }
    
    // Add queue levels
    g_mutex_lock(&data->stats_mutex);
    
    // Video queue
    json_builder_set_member_name(builder, "video_queue");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "percent_full");
    json_builder_add_double_value(builder, data->stats.input_video_queue.percent_full);
    json_builder_set_member_name(builder, "buffers");
    json_builder_add_int_value(builder, data->stats.input_video_queue.current_buffers);
    json_builder_set_member_name(builder, "bytes");
    json_builder_add_int_value(builder, data->stats.input_video_queue.current_bytes);
    json_builder_set_member_name(builder, "time_ms");
    json_builder_add_double_value(builder, data->stats.input_video_queue.current_time_ns / 1000000.0);
    json_builder_end_object(builder);
    
    // Audio queue
    json_builder_set_member_name(builder, "audio_queue");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "percent_full");
    json_builder_add_double_value(builder, data->stats.input_audio_queue.percent_full);
    json_builder_set_member_name(builder, "buffers");
    json_builder_add_int_value(builder, data->stats.input_audio_queue.current_buffers);
    json_builder_set_member_name(builder, "bytes");
    json_builder_add_int_value(builder, data->stats.input_audio_queue.current_bytes);
    json_builder_set_member_name(builder, "time_ms");
    json_builder_add_double_value(builder, data->stats.input_audio_queue.current_time_ns / 1000000.0);
    json_builder_end_object(builder);


    json_builder_set_member_name(builder, "output_queue");
json_builder_begin_object(builder);
json_builder_set_member_name(builder, "percent_full");
json_builder_add_double_value(builder, data->stats.output_queue.percent_full);
json_builder_set_member_name(builder, "buffers");
json_builder_add_int_value(builder, data->stats.output_queue.current_buffers);
json_builder_set_member_name(builder, "bytes");
json_builder_add_int_value(builder, data->stats.output_queue.current_bytes);
json_builder_set_member_name(builder, "time_ms");
json_builder_add_double_value(builder, data->stats.output_queue.current_time_ns / 1000000.0);
json_builder_end_object(builder);

     
        json_builder_set_member_name(builder, "audio_out_queue");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "percent_full");
        json_builder_add_double_value(builder, data->stats.audio_output_queue.percent_full);
        json_builder_set_member_name(builder, "buffers");
        json_builder_add_int_value(builder, data->stats.audio_output_queue.current_buffers);
        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, data->stats.audio_output_queue.current_bytes);
        json_builder_set_member_name(builder, "time_ms");
        json_builder_add_double_value(builder, data->stats.audio_output_queue.current_time_ns / 1000000.0);
        json_builder_end_object(builder);
    

    
    g_mutex_unlock(&data->stats_mutex);
    
    json_builder_end_object(builder);  // End buffer_info
    
    // Add processing metrics
    json_builder_set_member_name(builder, "processing_metrics");
    json_builder_begin_object(builder);
    
    g_mutex_lock(&data->stats_mutex);
    
    json_builder_set_member_name(builder, "frames_processed");
    json_builder_add_int_value(builder, data->stats.processing.frames_processed);
    
    json_builder_set_member_name(builder, "encoding_fps");
    json_builder_add_double_value(builder, data->stats.processing.video_encoding_fps);
    
    json_builder_set_member_name(builder, "end_to_end_latency_ms");
    json_builder_add_double_value(builder, data->stats.processing.end_to_end_latency_ms);
    
    json_builder_set_member_name(builder, "cpu_usage");
    json_builder_add_double_value(builder, data->stats.processing.cpu_usage_percent);
    
    json_builder_set_member_name(builder, "memory_usage_mb");
    json_builder_add_double_value(builder, data->stats.processing.memory_usage_bytes / (1024.0 * 1024.0));
    
    g_mutex_unlock(&data->stats_mutex);
    
    json_builder_end_object(builder);  // End processing_metrics
    
    // Add recovery info
    json_builder_set_member_name(builder, "recovery_info");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, data->watchdog_enabled);
    
    json_builder_set_member_name(builder, "timeout_seconds");
    json_builder_add_int_value(builder, data->watchdog_timeout_sec);
    
    json_builder_set_member_name(builder, "active");
    json_builder_add_boolean_value(builder, data->recovery.recovery_active);
    
    json_builder_set_member_name(builder, "attempt_count");
    json_builder_add_int_value(builder, data->recovery.recovery_attempt_count);
    
    g_mutex_lock(&data->stats_mutex);
    
    json_builder_set_member_name(builder, "network_stable");
    json_builder_add_boolean_value(builder, data->stats.network.network_stable);
    
    json_builder_set_member_name(builder, "reconnections");
    json_builder_add_int_value(builder, data->stats.network.successful_reconnections);
    
    json_builder_set_member_name(builder, "last_reconnection_time");
    json_builder_add_int_value(builder, data->stats.network.last_reconnection_time);
    
    g_mutex_unlock(&data->stats_mutex);
    
    json_builder_end_object(builder);  // End recovery_info
    
    json_builder_end_object(builder);  // End root object
    
    // Generate the JSON node
    root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    return root;
}

static JsonNode* create_metrics_json(AppData *data) {
    JsonBuilder *builder = json_builder_new();
    JsonNode *root = NULL;
    
    // Start building metrics JSON
    json_builder_begin_object(builder);
    
    // Add timestamp
    json_builder_set_member_name(builder, "timestamp_unix");
    json_builder_add_int_value(builder, g_get_real_time() / 1000000);
    
    // Processing metrics
    json_builder_set_member_name(builder, "processing");
    json_builder_begin_object(builder);
    
    g_mutex_lock(&data->stats_mutex);


     // Update create_metrics_json function - add stream_info section after processing section
json_builder_set_member_name(builder, "stream_info");
json_builder_begin_object(builder);

json_builder_set_member_name(builder, "video_pid");
json_builder_add_int_value(builder, data->discovered_input_video_pid);

json_builder_set_member_name(builder, "audio_pid");
json_builder_add_int_value(builder, data->discovered_input_audio_pid);

json_builder_set_member_name(builder, "program_number");
json_builder_add_int_value(builder, data->program_number >= 0 ? data->program_number : -1);

json_builder_set_member_name(builder, "video_codec");
json_builder_add_string_value(builder, data->discovered_input_video_codec ? 
                             data->discovered_input_video_codec : "unknown");

json_builder_set_member_name(builder, "audio_codec");
json_builder_add_string_value(builder, data->discovered_input_audio_codec ? 
                             data->discovered_input_audio_codec : "unknown");

json_builder_set_member_name(builder, "selection_method");
if (data->manual_video_pid >= 0 || data->manual_audio_pid >= 0) {
    json_builder_add_string_value(builder, "manual_pid");
} else if (data->program_number >= 0 && data->use_program_map) {
    json_builder_add_string_value(builder, "program_number");
} else {
    json_builder_add_string_value(builder, "auto");
}

json_builder_end_object(builder);  // End stream_info
    
    // Frame metrics
    json_builder_set_member_name(builder, "frames_processed");
    json_builder_add_int_value(builder, data->stats.processing.frames_processed);
    
    json_builder_set_member_name(builder, "frames_dropped");
    json_builder_add_int_value(builder, data->stats.processing.frames_dropped);
    
    json_builder_set_member_name(builder, "frames_delayed");
    json_builder_add_int_value(builder, data->stats.processing.frames_delayed);
    
   

// A/V sync
json_builder_set_member_name(builder, "av_sync");
json_builder_begin_object(builder);

json_builder_set_member_name(builder, "current_drift_ms");
json_builder_add_double_value(builder, data->stats.processing.audio_video_drift_ms);

json_builder_set_member_name(builder, "max_drift_ms");
json_builder_add_double_value(builder, data->stats.processing.max_drift_ms);

json_builder_set_member_name(builder, "last_audio_pts");
json_builder_add_int_value(builder, data->stats.processing.last_audio_pts);

json_builder_set_member_name(builder, "last_video_pts");
json_builder_add_int_value(builder, data->stats.processing.last_video_pts);

// Add time difference between last audio and video PTS
json_builder_set_member_name(builder, "pts_time_diff_ms");
json_builder_add_double_value(builder, data->stats.processing.audio_video_drift_ms);

json_builder_end_object(builder);  // End av_sync

    
    // Encoder metrics
    json_builder_set_member_name(builder, "encoding_fps");
    json_builder_add_double_value(builder, data->stats.processing.video_encoding_fps);
    
    json_builder_set_member_name(builder, "avg_qp_value");
    json_builder_add_double_value(builder, data->stats.processing.avg_qp_value);
    
    json_builder_set_member_name(builder, "min_qp_value");
    json_builder_add_double_value(builder, data->stats.processing.min_qp_value);
    
    json_builder_set_member_name(builder, "max_qp_value");
    json_builder_add_double_value(builder, data->stats.processing.max_qp_value);
    
    // Performance metrics
    json_builder_set_member_name(builder, "end_to_end_latency_ms");
    json_builder_add_double_value(builder, data->stats.processing.end_to_end_latency_ms);
    
    json_builder_set_member_name(builder, "cpu_usage_percent");
    json_builder_add_double_value(builder, data->stats.processing.cpu_usage_percent);
    
    json_builder_set_member_name(builder, "memory_usage_mb");
    json_builder_add_double_value(builder, data->stats.processing.memory_usage_bytes / (1024.0 * 1024.0));
    
    // Timestamp info
    json_builder_set_member_name(builder, "pts_discontinuity");
    json_builder_add_boolean_value(builder, data->stats.processing.pts_discontinuity);
    
    json_builder_set_member_name(builder, "timestamp_gap_ms");
    json_builder_add_double_value(builder, data->stats.processing.timestamp_gap_ns / 1000000.0);
    
    g_mutex_unlock(&data->stats_mutex);
    
    json_builder_end_object(builder);  // End processing
    
    // Network metrics
    json_builder_set_member_name(builder, "network");
    json_builder_begin_object(builder);
    
    g_mutex_lock(&data->stats_mutex);
    
    // Jitter metrics
    json_builder_set_member_name(builder, "input_jitter_ms");
    json_builder_add_double_value(builder, data->stats.network.input_jitter_ms);
    
    json_builder_set_member_name(builder, "output_jitter_ms");
    json_builder_add_double_value(builder, data->stats.network.output_jitter_ms);
    
    // Packet metrics
    json_builder_set_member_name(builder, "input_packets");
    json_builder_add_int_value(builder, data->stats.network.total_input_packets);
    
    json_builder_set_member_name(builder, "output_packets");
    json_builder_add_int_value(builder, data->stats.network.total_output_packets);
    
    json_builder_set_member_name(builder, "dropped_packets");
    json_builder_add_int_value(builder, data->stats.network.dropped_packets);
    
    json_builder_set_member_name(builder, "avg_packet_size_bytes");
    json_builder_add_double_value(builder, data->stats.network.avg_packet_size_bytes);
    
    // Network stability
    json_builder_set_member_name(builder, "network_stable");
    json_builder_add_boolean_value(builder, data->stats.network.network_stable);
    
    json_builder_set_member_name(builder, "reconnection_attempts");
    json_builder_add_int_value(builder, data->stats.network.reconnection_attempts);
    
    json_builder_set_member_name(builder, "successful_reconnections");
    json_builder_add_int_value(builder, data->stats.network.successful_reconnections);
    
    g_mutex_unlock(&data->stats_mutex);
    
    json_builder_end_object(builder);  // End network
    
    // Bitrates
    json_builder_set_member_name(builder, "bitrates");
    json_builder_begin_object(builder);
    
    g_mutex_lock(&data->stats_mutex);
    
    json_builder_set_member_name(builder, "input_mbps");
    json_builder_add_double_value(builder, data->stats.current_input_bitrate_bps / 1000000.0);
    
    json_builder_set_member_name(builder, "output_mbps");
    json_builder_add_double_value(builder, data->stats.current_output_bitrate_bps / 1000000.0);
    
    // Add historical data for graphs
    json_builder_set_member_name(builder, "history");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "input_mbps");
    json_builder_begin_array(builder);
    for (int i = 0; i < data->stats.num_history_samples; i++) {
        int idx = (data->stats.history_index - i - 1 + 60) % 60;
        json_builder_add_double_value(builder, data->stats.input_bitrates_history[idx]);
    }
    json_builder_end_array(builder);
    
    json_builder_set_member_name(builder, "output_mbps");
    json_builder_begin_array(builder);
    for (int i = 0; i < data->stats.num_history_samples; i++) {
        int idx = (data->stats.history_index - i - 1 + 60) % 60;
        json_builder_add_double_value(builder, data->stats.output_bitrates_history[idx]);
    }
    json_builder_end_array(builder);
    
    json_builder_end_object(builder);  // End history
    
    g_mutex_unlock(&data->stats_mutex);
    
    json_builder_end_object(builder);  // End bitrates
    
    json_builder_end_object(builder);  // End root
    
    // Generate JSON node
    root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    return root;
}

static JsonNode* create_buffer_levels_json(AppData *data) {
    JsonBuilder *builder = json_builder_new();
    JsonNode *root = NULL;
    
    // Start building buffer levels JSON
    json_builder_begin_object(builder);
    
    // Add timestamp
    json_builder_set_member_name(builder, "timestamp_unix");
    json_builder_add_int_value(builder, g_get_real_time() / 1000000);
    
    // Add configuration
    json_builder_set_member_name(builder, "configuration");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "buffer_mode");
    switch (data->buffer_mode) {
        case BUFFER_MODE_LOW_LATENCY:
            json_builder_add_string_value(builder, "low-latency");
            break;
        case BUFFER_MODE_HIGH_QUALITY:
            json_builder_add_string_value(builder, "high-quality");
            break;
        default:
            json_builder_add_string_value(builder, "default");
            break;
    }
    
    json_builder_set_member_name(builder, "leaky_mode");
    switch (data->leaky_mode) {
        case LEAKY_MODE_UPSTREAM:
            json_builder_add_string_value(builder, "upstream (drop oldest)");
            break;
        case LEAKY_MODE_DOWNSTREAM:
            json_builder_add_string_value(builder, "downstream (drop newest)");
            break;
        default:
            json_builder_add_string_value(builder, "none");
            break;
    }
    
    json_builder_set_member_name(builder, "buffer_size_mb");
    json_builder_add_int_value(builder, data->buffer_size_mb);
    
    json_builder_set_member_name(builder, "buffer_time_ms");
    json_builder_add_int_value(builder, data->buffer_size_time_ms);
    
    json_builder_end_object(builder);  // End configuration
    
    g_mutex_lock(&data->stats_mutex);
    
    // Add video queue data
    json_builder_set_member_name(builder, "queues");
    json_builder_begin_object(builder);
    
    // Video input queue
    json_builder_set_member_name(builder, "video_input");
    json_builder_begin_object(builder);
    
    // Current values
    json_builder_set_member_name(builder, "current");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "percent_full");
    json_builder_add_double_value(builder, data->stats.input_video_queue.percent_full);
    
    json_builder_set_member_name(builder, "buffers");
    json_builder_add_int_value(builder, data->stats.input_video_queue.current_buffers);
    
    json_builder_set_member_name(builder, "bytes");
    json_builder_add_int_value(builder, data->stats.input_video_queue.current_bytes);
    
    json_builder_set_member_name(builder, "time_ms");
    json_builder_add_double_value(builder, data->stats.input_video_queue.current_time_ns / 1000000.0);
    
    json_builder_end_object(builder);  // End current
    
    // Limits
    json_builder_set_member_name(builder, "limits");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "max_buffers");
    json_builder_add_int_value(builder, data->stats.input_video_queue.max_buffers);
    
    json_builder_set_member_name(builder, "max_bytes");
    json_builder_add_int_value(builder, data->stats.input_video_queue.max_bytes);
    
    json_builder_set_member_name(builder, "max_time_ms");
    json_builder_add_double_value(builder, data->stats.input_video_queue.max_time_ns / 1000000.0);
    
    json_builder_end_object(builder);  // End limits
    
    // Overflow statistics
    json_builder_set_member_name(builder, "overflows");
    json_builder_add_int_value(builder, data->stats.input_video_queue.overflow_count);
    
    json_builder_set_member_name(builder, "underflows");
    json_builder_add_int_value(builder, data->stats.input_video_queue.underflow_count);
    
    json_builder_end_object(builder);  // End video_input
    
    // Audio input queue
    json_builder_set_member_name(builder, "audio_input");
    json_builder_begin_object(builder);
    
    // Current values
    json_builder_set_member_name(builder, "current");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "percent_full");
    json_builder_add_double_value(builder, data->stats.input_audio_queue.percent_full);
    
    json_builder_set_member_name(builder, "buffers");
    json_builder_add_int_value(builder, data->stats.input_audio_queue.current_buffers);
    
    json_builder_set_member_name(builder, "bytes");
    json_builder_add_int_value(builder, data->stats.input_audio_queue.current_bytes);
    
    json_builder_set_member_name(builder, "time_ms");
    json_builder_add_double_value(builder, data->stats.input_audio_queue.current_time_ns / 1000000.0);
    
    json_builder_end_object(builder);  // End current
    
    // Limits
    json_builder_set_member_name(builder, "limits");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "max_buffers");
    json_builder_add_int_value(builder, data->stats.input_audio_queue.max_buffers);
    
    json_builder_set_member_name(builder, "max_bytes");
    json_builder_add_int_value(builder, data->stats.input_audio_queue.max_bytes);
    
    json_builder_set_member_name(builder, "max_time_ms");
    json_builder_add_double_value(builder, data->stats.input_audio_queue.max_time_ns / 1000000.0);
    
    json_builder_end_object(builder);  // End limits
    
    // Overflow statistics
    json_builder_set_member_name(builder, "overflows");
    json_builder_add_int_value(builder, data->stats.input_audio_queue.overflow_count);
    
    json_builder_set_member_name(builder, "underflows");
    json_builder_add_int_value(builder, data->stats.input_audio_queue.underflow_count);
    
    json_builder_end_object(builder);  // End audio_input
    
    // Output video queue - only add if it exists
    if (data->video_out_queue) {
        json_builder_set_member_name(builder, "video_output");
        json_builder_begin_object(builder);
        
        // Current values
        json_builder_set_member_name(builder, "current");
        json_builder_begin_object(builder);
        
        json_builder_set_member_name(builder, "percent_full");
        json_builder_add_double_value(builder, data->stats.output_queue.percent_full);
        
        json_builder_set_member_name(builder, "buffers");
        json_builder_add_int_value(builder, data->stats.output_queue.current_buffers);
        
        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, data->stats.output_queue.current_bytes);
        
        json_builder_set_member_name(builder, "time_ms");
        json_builder_add_double_value(builder, data->stats.output_queue.current_time_ns / 1000000.0);
        
        json_builder_end_object(builder);  // End current
        
        // Limits
        json_builder_set_member_name(builder, "limits");
        json_builder_begin_object(builder);
        
        json_builder_set_member_name(builder, "max_buffers");
        json_builder_add_int_value(builder, data->stats.output_queue.max_buffers);
        
        json_builder_set_member_name(builder, "max_bytes");
        json_builder_add_int_value(builder, data->stats.output_queue.max_bytes);
        
        json_builder_set_member_name(builder, "max_time_ms");
        json_builder_add_double_value(builder, data->stats.output_queue.max_time_ns / 1000000.0);
        
        json_builder_end_object(builder);  // End limits
        
        // Overflow statistics
        json_builder_set_member_name(builder, "overflows");
        json_builder_add_int_value(builder, data->stats.output_queue.overflow_count);
        
        json_builder_set_member_name(builder, "underflows");
        json_builder_add_int_value(builder, data->stats.output_queue.underflow_count);
        
        json_builder_end_object(builder);  // End video_output
    }

        // Audio output queue - only add if it exists
    if (data->audio_out_queue) {
        json_builder_set_member_name(builder, "audio_output");
        json_builder_begin_object(builder);
        
        // Current values
        json_builder_set_member_name(builder, "current");
        json_builder_begin_object(builder);
        
        json_builder_set_member_name(builder, "percent_full");
        json_builder_add_double_value(builder, data->stats.audio_output_queue.percent_full);
        
        json_builder_set_member_name(builder, "buffers");
        json_builder_add_int_value(builder, data->stats.audio_output_queue.current_buffers);
        
        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, data->stats.audio_output_queue.current_bytes);
        
        json_builder_set_member_name(builder, "time_ms");
        json_builder_add_double_value(builder, data->stats.audio_output_queue.current_time_ns / 1000000.0);
        
        json_builder_end_object(builder);  // End current
        
        // Limits
        json_builder_set_member_name(builder, "limits");
        json_builder_begin_object(builder);
        
        json_builder_set_member_name(builder, "max_buffers");
        json_builder_add_int_value(builder, data->stats.audio_output_queue.max_buffers);
        
        json_builder_set_member_name(builder, "max_bytes");
        json_builder_add_int_value(builder, data->stats.audio_output_queue.max_bytes);
        
        json_builder_set_member_name(builder, "max_time_ms");
        json_builder_add_double_value(builder, data->stats.audio_output_queue.max_time_ns / 1000000.0);
        
        json_builder_end_object(builder);  // End limits
        
        // Overflow statistics
        json_builder_set_member_name(builder, "overflows");
        json_builder_add_int_value(builder, data->stats.audio_output_queue.overflow_count);
        
        json_builder_set_member_name(builder, "underflows");
        json_builder_add_int_value(builder, data->stats.audio_output_queue.underflow_count);
        
        json_builder_end_object(builder);  // End audio_output
    }
    
    json_builder_end_object(builder);  // End queues
    
    g_mutex_unlock(&data->stats_mutex);
    
    json_builder_end_object(builder);  // End root
    
    // Generate JSON node
    root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    return root;
}


// --- Stats Server Callback (Enhanced) ---
static void stats_server_callback(SoupServer *s, SoupMessage *m, const char *p, GHashTable *q, SoupClientContext *c, gpointer u) { 
    AppData *d = (AppData *)u;
    JsonNode *json_node = NULL;
    JsonGenerator *generator = NULL;
    gchar *json_str = NULL;
    
    // Only handle GET requests
    if (m->method != SOUP_METHOD_GET) {
        soup_message_set_status(m, SOUP_STATUS_METHOD_NOT_ALLOWED);
        return;
    }
    
    // Calculate latest bitrates before responding
    calculate_bitrates(d);
    
    // Create appropriate JSON response based on endpoint
    if (g_strcmp0(p, "/stats") == 0) {
        json_node = create_stats_json(d);
    } else if (g_strcmp0(p, "/metrics") == 0) {
        json_node = create_metrics_json(d);
    } else if (g_strcmp0(p, "/buffers") == 0) {
        json_node = create_buffer_levels_json(d);
    } else {
        soup_message_set_status(m, SOUP_STATUS_NOT_FOUND);
        return;
    }
    
    if (!json_node) {
        const gchar *error_json = "{\"error\":\"Failed to generate JSON\"}";
        soup_message_set_response(m, "application/json", SOUP_MEMORY_COPY, 
                                 error_json, strlen(error_json));
        soup_message_set_status(m, SOUP_STATUS_INTERNAL_SERVER_ERROR);
        return;
    }
    
    // Generate JSON string
    generator = json_generator_new();
    json_generator_set_root(generator, json_node);
    json_generator_set_pretty(generator, TRUE);
    json_str = json_generator_to_data(generator, NULL);
    
    // Set response headers
    soup_message_set_status(m, SOUP_STATUS_OK);
    soup_message_headers_append(m->response_headers, "Content-Type", "application/json; charset=utf-8");
    // Add CORS headers to allow access from web browsers
    soup_message_headers_append(m->response_headers, "Access-Control-Allow-Origin", "*");
    soup_message_headers_append(m->response_headers, "Access-Control-Allow-Methods", "GET, OPTIONS");
    
    // Set response body
    if (json_str) {
        soup_message_set_response(m, "application/json", SOUP_MEMORY_COPY, json_str, strlen(json_str));
        g_free(json_str);
    } else {
        const gchar *error_json = "{\"error\":\"Failed to generate JSON string\"}";
        soup_message_set_response(m, "application/json", SOUP_MEMORY_COPY, 
                                 error_json, strlen(error_json));
    }
    
    // Clean up
    if (generator) g_object_unref(generator);
    if (json_node) json_node_free(json_node);
}
