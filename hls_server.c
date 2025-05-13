/*
 * Simple HLS Server with codec detection and watchdog
 */
#include <gst/gst.h>
#include <glib.h>
#include <signal.h>

typedef struct {
    GstElement *pipeline;
    GMainLoop *main_loop;
    gchar *uri;
    gchar *hls_path;
    gchar *hls_playlist;
    gint target_duration;
    gint max_files;
    gint watchdog_timeout;
    
    // Codec detection
    gboolean detection_done;
    gchar *video_codec;
    gchar *audio_codec;
    GMutex lock;
    GCond cond;
    guint detect_timeout_id;
} AppData;

static AppData app = {0};

static void cleanup_and_exit(int sig);
static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data);
static void on_demux_pad_added(GstElement *element, GstPad *pad, gpointer data);
static gboolean detect_timeout(gpointer data);
static const gchar* get_parser_for_codec(const gchar *codec);
static gboolean start_hls_pipeline(AppData *app);
static gboolean restart_pipeline(gpointer data);

int main(int argc, char *argv[]) {
    GOptionContext *context;
    GError *error = NULL;
    
    // Command line options
    GOptionEntry entries[] = {
        { "uri", 'i', 0, G_OPTION_ARG_STRING, &app.uri, 
          "UDP input URI (udp://239.x.x.x:port)", "URI" },
        { "hls-path", 'o', 0, G_OPTION_ARG_STRING, &app.hls_path, 
          "HLS output path", "PATH" },
        { "hls-playlist", 'p', 0, G_OPTION_ARG_STRING, &app.hls_playlist, 
          "HLS playlist filename", "FILE" },
        { "target-duration", 'd', 0, G_OPTION_ARG_INT, &app.target_duration, 
          "HLS target duration in seconds", "SECONDS" },
        { "max-files", 'm', 0, G_OPTION_ARG_INT, &app.max_files, 
          "Maximum number of files to keep", "COUNT" },
        { "watchdog-timeout", 'w', 0, G_OPTION_ARG_INT, &app.watchdog_timeout,
          "Watchdog timeout in seconds", "SECONDS" },
        { NULL }
    };
    
    // Initialize GStreamer
    gst_init(&argc, &argv);
    
    // Set defaults
    app.uri = g_strdup("udp://239.7.2.2:59500");
    app.hls_path = g_strdup("/var/www/html/content/test");
    app.hls_playlist = g_strdup("playlist.m3u8");
    app.target_duration = 6;
    app.max_files = 6;
    app.watchdog_timeout = 5;
    app.detection_done = FALSE;
    app.video_codec = NULL;
    app.audio_codec = NULL;
    g_mutex_init(&app.lock);
    g_cond_init(&app.cond);
    
    // Parse options
    context = g_option_context_new("- Multicast to HLS Server");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    g_option_context_free(context);
    
    // Setup signal handlers
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    // Create main loop
    app.main_loop = g_main_loop_new(NULL, FALSE);
    
    // Create output directory
    g_mkdir_with_parents(app.hls_path, 0755);
    
    // Parse URI to extract address and port
    gchar *address = NULL;
    gint port = 0;
    
    gchar **uri_parts = g_strsplit(app.uri, "://", 2);
    if (uri_parts[1]) {
        gchar **addr_parts = g_strsplit(uri_parts[1], ":", 2);
        if (addr_parts[0] && addr_parts[1]) {
            address = g_strdup(addr_parts[0]);
            port = atoi(addr_parts[1]);
        }
        g_strfreev(addr_parts);
    }
    g_strfreev(uri_parts);
    
    if (!address || port == 0) {
        g_printerr("Invalid URI format: %s\n", app.uri);
        g_free(address);
        return 1;
    }
    
    g_print("Detecting codecs from stream...\n");
    
    // Create a simple pipeline for codec detection
    gchar *detect_pipeline_str = g_strdup_printf(
        "udpsrc address=%s port=%d buffer-size=2097152 ! tsdemux name=demux",
        address, port);
    
    g_free(address);
    
    GstElement *detect_pipeline = gst_parse_launch(detect_pipeline_str, &error);
    g_free(detect_pipeline_str);
    
    if (!detect_pipeline) {
        g_printerr("Failed to create detection pipeline: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    
    // Get the demuxer element
    GstElement *demux = gst_bin_get_by_name(GST_BIN(detect_pipeline), "demux");
    if (!demux) {
        g_printerr("Failed to get demuxer element\n");
        gst_object_unref(detect_pipeline);
        return 1;
    }
    
    // Connect pad-added signal for codec detection
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_demux_pad_added), &app);
    gst_object_unref(demux);
    
    // Add message handler
    GstBus *bus = gst_element_get_bus(detect_pipeline);
    gst_bus_add_watch(bus, bus_callback, &app);
    gst_object_unref(bus);
    
    // Start the detection pipeline
    gst_element_set_state(detect_pipeline, GST_STATE_PLAYING);
    
    // Set a timeout for detection (5 seconds)
    app.detect_timeout_id = g_timeout_add_seconds(5, detect_timeout, &app);
    
    // Wait for codec detection to complete or timeout
    g_mutex_lock(&app.lock);
    while (!app.detection_done) {
        g_cond_wait(&app.cond, &app.lock);
    }
    g_mutex_unlock(&app.lock);
    
    // Stop and free the detection pipeline
    gst_element_set_state(detect_pipeline, GST_STATE_NULL);
    gst_object_unref(detect_pipeline);
    
    if (app.detect_timeout_id) {
        g_source_remove(app.detect_timeout_id);
        app.detect_timeout_id = 0;
    }
    
    // Print detected codecs
    g_print("Detected video codec: %s\n", app.video_codec ? app.video_codec : "unknown");
    g_print("Detected audio codec: %s\n", app.audio_codec ? app.audio_codec : "unknown");
    
    // Use default codecs if not detected
    if (!app.video_codec) {
        app.video_codec = g_strdup("h264");
        g_print("Using default video codec: h264\n");
    }
    
    if (!app.audio_codec) {
        app.audio_codec = g_strdup("aac");
        g_print("Using default audio codec: aac\n");
    }
    
    // Start the actual HLS pipeline
    if (!start_hls_pipeline(&app)) {
        return 1;
    }
    
    g_print("HLS Server Started\n");
    g_print("  URI: %s\n", app.uri);
    g_print("  HLS Path: %s\n", app.hls_path);
    g_print("  Playlist: %s\n", app.hls_playlist);
    g_print("  Target Duration: %d seconds\n", app.target_duration);
    g_print("  Max Files: %d\n", app.max_files);
    g_print("  Watchdog Timeout: %d seconds\n", app.watchdog_timeout);
    g_print("Press Ctrl+C to quit\n");
    
    // Run the main loop
    g_main_loop_run(app.main_loop);
    
    // Cleanup
    cleanup_and_exit(0);
    
    return 0;
}

static void on_demux_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    AppData *app = (AppData *)data;
    GstCaps *caps;
    GstStructure *str;
    const gchar *name;
    
    // Get pad caps
    caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, NULL);
    }
    
    if (!caps) {
        g_print("No caps on pad\n");
        return;
    }
    
    str = gst_caps_get_structure(caps, 0);
    name = gst_structure_get_name(str);
    
    g_print("Demuxer pad added with caps: %s\n", name);
    
    g_mutex_lock(&app->lock);
    
    // Check if it's video or audio
    if (g_str_has_prefix(name, "video/")) {
        // Detect video codec
        if (g_str_has_prefix(name, "video/x-h264")) {
            if (!app->video_codec) {
                app->video_codec = g_strdup("h264");
                g_print("Detected video codec: h264\n");
            }
        } else if (g_str_has_prefix(name, "video/x-h265")) {
            if (!app->video_codec) {
                app->video_codec = g_strdup("h265");
                g_print("Detected video codec: h265\n");
            }
        } else if (g_str_has_prefix(name, "video/mpeg")) {
            gint mpegversion = 0;
            gst_structure_get_int(str, "mpegversion", &mpegversion);
            if (mpegversion == 2) {
                if (!app->video_codec) {
                    app->video_codec = g_strdup("mpeg2");
                    g_print("Detected video codec: mpeg2\n");
                }
            }
        }
    } else if (g_str_has_prefix(name, "audio/")) {
        // Detect audio codec
        if (g_str_has_prefix(name, "audio/mpeg")) {
            gint mpegversion = 0;
            gst_structure_get_int(str, "mpegversion", &mpegversion);
            if (mpegversion == 4) {
                if (!app->audio_codec) {
                    app->audio_codec = g_strdup("aac");
                    g_print("Detected audio codec: aac\n");
                }
            } else if (mpegversion == 1) {
                if (!app->audio_codec) {
                    app->audio_codec = g_strdup("mp2");
                    g_print("Detected audio codec: mp2\n");
                }
            }
        } else if (g_str_has_prefix(name, "audio/x-aac")) {
            if (!app->audio_codec) {
                app->audio_codec = g_strdup("aac");
                g_print("Detected audio codec: aac\n");
            }
        } else if (g_str_has_prefix(name, "audio/x-ac3")) {
            if (!app->audio_codec) {
                app->audio_codec = g_strdup("ac3");
                g_print("Detected audio codec: ac3\n");
            }
        }
    }
    
    // If we have both codecs, signal that detection is done
    if (app->video_codec && app->audio_codec) {
        app->detection_done = TRUE;
        g_cond_signal(&app->cond);
    }
    
    g_mutex_unlock(&app->lock);
    
    gst_caps_unref(caps);
}

static gboolean detect_timeout(gpointer data) {
    AppData *app = (AppData *)data;
    
    g_mutex_lock(&app->lock);
    app->detection_done = TRUE;
    g_cond_signal(&app->cond);
    g_mutex_unlock(&app->lock);
    
    g_print("Codec detection timed out\n");
    
    app->detect_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static const gchar* get_parser_for_codec(const gchar *codec) {
    if (!codec)
        return "h264parse";
        
    if (g_str_equal(codec, "h264"))
        return "h264parse";
    else if (g_str_equal(codec, "h265"))
        return "h265parse";
    else if (g_str_equal(codec, "mpeg2"))
        return "mpegvideoparse";
    else if (g_str_equal(codec, "aac"))
        return "aacparse";
    else if (g_str_equal(codec, "mp2"))
        return "mpegaudioparse";
    else if (g_str_equal(codec, "ac3"))
        return "ac3parse";
    
    return "identity";
}

static gboolean start_hls_pipeline(AppData *app) {
    GError *error = NULL;
    gchar *pipeline_str = NULL;
    gchar *address = NULL;
    gint port = 0;
    
    // Parse URI to extract address and port
    gchar **uri_parts = g_strsplit(app->uri, "://", 2);
    if (uri_parts[1]) {
        gchar **addr_parts = g_strsplit(uri_parts[1], ":", 2);
        if (addr_parts[0] && addr_parts[1]) {
            address = g_strdup(addr_parts[0]);
            port = atoi(addr_parts[1]);
        }
        g_strfreev(addr_parts);
    }
    g_strfreev(uri_parts);
    
    if (!address || port == 0) {
        g_printerr("Invalid URI format: %s\n", app->uri);
        g_free(address);
        return FALSE;
    }
    
    // Get parsers for detected codecs
    const gchar *video_parser = get_parser_for_codec(app->video_codec);
    const gchar *audio_parser = get_parser_for_codec(app->audio_codec);
    
    g_print("Using parsers: %s and %s\n", video_parser, audio_parser);
    
    // Create pipeline string with watchdog element
    pipeline_str = g_strdup_printf(
        "udpsrc address=%s port=%d buffer-size=2097152 ! "
        "tsdemux name=demux "
        "demux. ! queue max-size-buffers=1000 max-size-bytes=10485760 max-size-time=10000000000 ! "
        "%s config-interval=1 ! queue ! mux. "
        "demux. ! queue max-size-buffers=1000 max-size-bytes=10485760 max-size-time=10000000000 ! "
        "%s ! queue ! mux. "
        "mpegtsmux name=mux alignment=7 ! "
        "watchdog timeout=%d ! "
        "hlssink location=\"%s/segment%%05d.ts\" "
        "playlist-location=\"%s/%s\" "
        "target-duration=%d max-files=%d",
        address, port,
        video_parser, audio_parser,
        app->watchdog_timeout * 1000, // convert to milliseconds
        app->hls_path, app->hls_path, app->hls_playlist,
        app->target_duration, app->max_files);
    
    g_free(address);
    
    g_print("Pipeline: %s\n", pipeline_str);
    
    // Create pipeline
    app->pipeline = gst_parse_launch(pipeline_str, &error);
    g_free(pipeline_str);
    
    if (!app->pipeline) {
        g_printerr("Failed to create HLS pipeline: %s\n", error->message);
        g_error_free(error);
        return FALSE;
    }
    
    // Add message handler
    GstBus *bus = gst_element_get_bus(app->pipeline);
    gst_bus_add_watch(bus, bus_callback, app);
    gst_object_unref(bus);
    
    // Start the pipeline
    gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    
    return TRUE;
}

static gboolean restart_pipeline(gpointer data) {
    AppData *app = (AppData *)data;
    
    g_print("Restarting pipeline due to watchdog timeout...\n");
    
    // Stop current pipeline
    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
    }
    
    // Start a new pipeline
    start_hls_pipeline(app);
    
    return G_SOURCE_REMOVE;
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data) {
    AppData *app = (AppData *)data;
    
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            
            gst_message_parse_error(message, &err, &debug);
            g_printerr("Error: %s\n", err->message);
            g_printerr("Debug: %s\n", debug ? debug : "none");
            g_error_free(err);
            g_free(debug);
            
            // Don't quit on errors during detection
            if (app->detection_done && app->pipeline) {
                // Restart the pipeline instead of quitting
                g_timeout_add_seconds(1, restart_pipeline, app);
            }
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            if (app->detection_done) {
                g_main_loop_quit(app->main_loop);
            }
            break;
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(message);
            if (s && g_str_equal(gst_structure_get_name(s), "GstWatchdogMessage")) {
                g_print("Watchdog timeout detected!\n");
                g_timeout_add_seconds(1, restart_pipeline, app);
            }
            break;
        }
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(app->pipeline)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);
                g_print("Pipeline state changed from %s to %s\n",
                       gst_element_state_get_name(old_state),
                       gst_element_state_get_name(new_state));
            }
            break;
        default:
            break;
    }
    
    return TRUE;
}

static void cleanup_and_exit(int sig) {
    g_print("Stopping HLS Server...\n");
    
    // Stop the main loop
    if (app.main_loop && g_main_loop_is_running(app.main_loop))
        g_main_loop_quit(app.main_loop);
    
    // Stop the pipeline
    if (app.pipeline) {
        gst_element_set_state(app.pipeline, GST_STATE_NULL);
        gst_object_unref(app.pipeline);
    }
    
    // Free resources
    if (app.main_loop)
        g_main_loop_unref(app.main_loop);
    
    g_free(app.uri);
    g_free(app.hls_path);
    g_free(app.hls_playlist);
    g_free(app.video_codec);
    g_free(app.audio_codec);
    
    g_mutex_clear(&app.lock);
    g_cond_clear(&app.cond);
    
    g_print("HLS Server stopped\n");
    
    if (sig)
        exit(0);
}