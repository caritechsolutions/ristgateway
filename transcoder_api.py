from fastapi import FastAPI, HTTPException, Query, Body, Depends, APIRouter
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
import subprocess
import requests
import json
import time
import os
import re
import yaml
import logging
from typing import Dict, Optional, List, Any
import datetime

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler("/var/log/transcoder-api.log"),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

# Constants
SERVICE_DIR = "/etc/systemd/system"
TRANSCODER_CONFIG_FILE = "transcoder_config.yaml"
DEVICE_CACHE_TIMEOUT = 300  # 5 minutes in seconds

# Initialize router
router = APIRouter(prefix="/transcoders", tags=["transcoders"])

# Devices cache
devices_cache = {
    "timestamp": 0,
    "devices": []
}

# Models for Transcoder API
class TranscoderInput(BaseModel):
    address: str
    program_pid: Optional[int] = Field(default=-1, description="-1 means auto-detect")
    video_pid: int
    audio_pid: int

class VideoConfig(BaseModel):
    device: str = "cpu"
    codec: str
    resolution: str
    b_frames: int = 0
    profile: str
    preset: str
    deinterlace: bool = False
    bitrate: int  # kbps
    keyframe_interval: int = 60

class AudioConfig(BaseModel):
    codec: str
    sample_rate: str
    channels: str
    bitrate: int  # kbps

class TranscoderOutput(BaseModel):
    address: str
    program_pid: Optional[int] = None
    video_pid: Optional[int] = None
    audio_pid: Optional[int] = None
    mux_bitrate: Optional[int] = None

class BufferSettings(BaseModel):
    buffer_mode: int = 0  # 0=default, 1=low-latency, 2=high-quality
    leaky_mode: int = 0   # 0=none, 1=upstream, 2=downstream
    buffer_size_mb: int = 4
    buffer_time_ms: int = 500

class AdvancedSettings(BaseModel):
    watchdog_enabled: bool = False
    watchdog_timeout: int = 10
    add_clock_overlay: bool = False

class TranscoderChannel(BaseModel):
    name: str
    input: TranscoderInput
    video: VideoConfig
    audio: AudioConfig
    output: TranscoderOutput
    buffer_settings: Optional[BufferSettings] = Field(default_factory=BufferSettings)
    advanced_settings: Optional[AdvancedSettings] = Field(default_factory=AdvancedSettings)
    enabled: bool = True
    status: str = "stopped"
    metrics_port: Optional[int] = None

class QueueLevelData(BaseModel):
    current_buffers: int = 0
    current_bytes: int = 0
    current_time_ns: int = 0
    max_buffers: int = 0
    max_bytes: int = 0
    max_time_ns: int = 0
    overflow_count: int = 0
    underflow_count: int = 0
    percent_full: float = 0.0

class ProcessingMetrics(BaseModel):
    frames_processed: int = 0
    frames_dropped: int = 0
    frames_delayed: int = 0
    avg_qp_value: float = 0.0
    min_qp_value: float = 0.0
    max_qp_value: float = 0.0
    avg_encoding_time_ms: float = 0.0
    audio_video_drift_ms: float = 0.0
    last_audio_pts: int = 0
    last_video_pts: int = 0
    max_drift_ms: float = 0.0
    cpu_usage_percent: float = 0.0
    memory_usage_bytes: int = 0
    video_encoding_fps: float = 0.0
    end_to_end_latency_ms: float = 0.0
    last_input_pts: int = 0
    last_output_pts: int = 0
    timestamp_gap_ns: int = 0
    pts_discontinuity: bool = False

class NetworkMetrics(BaseModel):
    input_jitter_ms: float = 0.0
    output_jitter_ms: float = 0.0
    total_input_packets: int = 0
    total_output_packets: int = 0
    dropped_packets: int = 0
    avg_packet_size_bytes: float = 0.0
    reconnection_attempts: int = 0
    successful_reconnections: int = 0
    last_reconnection_time: int = 0
    network_stable: bool = True

class TranscoderMetrics(BaseModel):
    cpu_usage: float = 0.0
    memory_usage: float = 0.0
    output_bitrate: int = 0
    input_bitrate: int = 0
    input_bitrate_mbps: float = 0.0
    output_bitrate_mbps: float = 0.0
    frames_processed: int = 0
    dropped_frames: int = 0
    status: str = "stopped"
    uptime: int = 0
    timestamp: str = ""
    video_codec: str = ""
    video_bitrate_kbps: int = 0
    audio_codec: str = ""
    audio_bitrate_kbps: int = 0
    packets: Dict[str, int] = Field(default_factory=dict)
    processing: Optional[ProcessingMetrics] = None
    network: Optional[NetworkMetrics] = None
    av_sync: Optional[Dict[str, Any]] = None
    bitrate_history: Optional[Dict[str, List[float]]] = None
    input_video_queue: Optional[QueueLevelData] = None
    input_audio_queue: Optional[QueueLevelData] = None
    output_queue: Optional[QueueLevelData] = None
    audio_output_queue: Optional[QueueLevelData] = None

# Add the TranscoderStatus model that was missing
class TranscoderStatus(BaseModel):
    cpu_usage: float
    memory_usage: float
    output_bitrate: int
    frames_processed: int
    dropped_frames: int
    status: str
    uptime: int  # seconds
    timestamp: str

# Helper Functions
def load_config(file_path):
    """Load configuration from YAML file"""
    if not os.path.exists(file_path):
        return {"transcoders": {}}
    with open(file_path, 'r') as f:
        return yaml.safe_load(f) or {"transcoders": {}}

def save_config(config, file_path):
    """Save configuration to YAML file"""
    # Ensure directory exists
    os.makedirs(os.path.dirname(file_path) if os.path.dirname(file_path) else '.', exist_ok=True)
    with open(file_path, 'w') as f:
        yaml.dump(config, f)

def get_next_transcoder_id():
    """Get the next available transcoder ID"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    
    # Find the highest transcoder number
    max_num = 0
    for transcoder_id in config.get("transcoders", {}).keys():
        if transcoder_id.startswith('transcoder'):
            try:
                num = int(transcoder_id.replace('transcoder', ''))
                if num > max_num:
                    max_num = num
            except ValueError:
                continue
    
    return f"transcoder{max_num + 1}"

def get_next_transcoder_metrics_port():
    """Find the next available metrics port for transcoders"""
    max_port = 9900  # Starting port number for transcoders
    
    # Check existing transcoder config
    config = load_config(TRANSCODER_CONFIG_FILE)
    for transcoder in config.get("transcoders", {}).values():
        if transcoder.get("metrics_port", 0) > max_port:
            max_port = transcoder["metrics_port"]
    
    return max_port + 1

def detect_nvidia_gpus():
    """Detect NVIDIA GPUs with NVENC support"""
    devices = []
    try:
        # Check if nvidia-smi is available
        result = subprocess.run(["nvidia-smi", "-L"], capture_output=True, text=True, check=False)
        if result.returncode == 0:
            # Parse the output to get GPU models
            gpu_lines = result.stdout.strip().split('\n')
            for i, line in enumerate(gpu_lines):
                # Extract GPU name from the output
                match = re.search(r'GPU \d+: (.+?) \(', line)
                if match:
                    gpu_name = match.group(1)
                    devices.append({
                        "id": f"nvidia_{i}",
                        "name": f"NVIDIA {gpu_name}",
                        "type": "nvidia"
                    })
        
        # Also check GStreamer's nvenc plugin availability
        gst_check = subprocess.run(
            ["gst-inspect-1.0", "nvenc"], 
            capture_output=True, 
            text=True, 
            check=False
        )
        
        if gst_check.returncode != 0:
            # If GStreamer nvenc plugin is not available, clear the devices list
            logger.warning("NVIDIA GPUs detected but GStreamer nvenc plugin is not available")
            devices = []
            
    except Exception as e:
        logger.error(f"Error detecting NVIDIA GPUs: {e}")
    
    return devices

def detect_intel_gpus():
    """Detect Intel GPUs with QuickSync support"""
    devices = []
    try:
        # Check for Intel GPU via vainfo
        result = subprocess.run(
            ["vainfo"], 
            capture_output=True, 
            text=True, 
            check=False
        )
        
        if result.returncode == 0 and "Intel" in result.stdout:
            # Further check for specific encoding capabilities
            if "H264" in result.stdout or "VAEncH264" in result.stdout:
                devices.append({
                    "id": "intel_vaapi",
                    "name": "Intel QuickSync",
                    "type": "intel"
                })
        
        # Also check if GStreamer vaapi plugin is available
        gst_check = subprocess.run(
            ["gst-inspect-1.0", "vaapi"],
            capture_output=True,
            text=True,
            check=False
        )
        
        if gst_check.returncode != 0:
            # If GStreamer vaapi plugin is not available, clear the devices list
            logger.warning("Intel GPU detected but GStreamer vaapi plugin is not available")
            devices = []
            
    except Exception as e:
        logger.error(f"Error detecting Intel GPUs: {e}")
    
    return devices

def detect_amd_gpus():
    """Detect AMD GPUs with encoding support"""
    devices = []
    try:
        # Check for AMD GPU via lspci
        result = subprocess.run(
            ["lspci", "-v"], 
            capture_output=True, 
            text=True, 
            check=False
        )
        
        if result.returncode == 0 and ("AMD" in result.stdout or "Radeon" in result.stdout):
            # Check for vaapi support
            vainfo = subprocess.run(
                ["vainfo"], 
                capture_output=True, 
                text=True, 
                check=False
            )
            
            if vainfo.returncode == 0 and ("AMD" in vainfo.stdout or "Radeon" in vainfo.stdout):
                devices.append({
                    "id": "amd_vaapi",
                    "name": "AMD VAAPI",
                    "type": "amd"
                })
        
        # Check if GStreamer AMF plugin is available (for Windows) or VAAPI for Linux
        gst_check = subprocess.run(
            ["gst-inspect-1.0", "vaapi"], 
            capture_output=True, 
            text=True, 
            check=False
        )
        
        if gst_check.returncode != 0:
            # If GStreamer plugin is not available, clear the devices list
            logger.warning("AMD GPU detected but necessary GStreamer plugin is not available")
            devices = []
            
    except Exception as e:
        logger.error(f"Error detecting AMD GPUs: {e}")
    
    return devices

def generate_transcoder_service_file(transcoder_id, config):
    """Generate systemd service file for the transcoder using the new binary"""
    
    # Get configuration
    input_address = config["input"]["address"]
    output_address = config["output"]["address"]
    video_codec = config["video"]["codec"]
    video_bitrate = config["video"]["bitrate"]
    audio_codec = config["audio"]["codec"]
    audio_bitrate = config["audio"]["bitrate"]
    
    # Get metrics port (with fallback)
    metrics_port = config.get("metrics_port", 9999)
    
    # Build command with all new options
    cmd_parts = [
        "/root/ristgateway/basic_transcoder",
        "--input-uri", input_address,
        "--output-uri", output_address,
        "--video-codec", video_codec,
        "--video-bitrate", str(video_bitrate),
        "--audio-codec", audio_codec,
        "--audio-bitrate", str(audio_bitrate),
        "--stats-port", str(metrics_port),
    ]
    
    # Add PID selections
    if config["input"].get("program_pid", -1) >= 0:
        cmd_parts.extend(["--program", str(config["input"]["program_pid"])])
    if config["input"].get("video_pid"):
        cmd_parts.extend(["--video-pid", str(config["input"]["video_pid"])])
    if config["input"].get("audio_pid"):
        cmd_parts.extend(["--audio-pid", str(config["input"]["audio_pid"])])
    
    # Add buffer settings
    if "buffer_settings" in config:
        buffer_settings = config["buffer_settings"]
        cmd_parts.extend(["--buffer-mode", str(buffer_settings.get("buffer_mode", 0))])
        cmd_parts.extend(["--leaky-mode", str(buffer_settings.get("leaky_mode", 0))])
        cmd_parts.extend(["--buffer-size", str(buffer_settings.get("buffer_size_mb", 4))])
        cmd_parts.extend(["--buffer-time", str(buffer_settings.get("buffer_time_ms", 500))])
    
    # Add video settings
    if config["video"].get("preset"):
        cmd_parts.extend(["--preset", config["video"]["preset"]])
    if config["video"].get("keyframe_interval"):
        cmd_parts.extend(["--keyframe-interval", str(config["video"]["keyframe_interval"])])
    if config["video"].get("deinterlace"):
        cmd_parts.append("--deinterlace")
    
    # Add advanced settings
    if "advanced_settings" in config:
        advanced = config["advanced_settings"]
        if advanced.get("watchdog_enabled"):
            cmd_parts.append("--watchdog")
            if advanced.get("watchdog_timeout"):
                cmd_parts.extend(["--watchdog-timeout", str(advanced["watchdog_timeout"])])
        if advanced.get("add_clock_overlay"):
            cmd_parts.append("--add-clock")
    
    # Create service file
    service_template = f"""[Unit]
Description=Binary Transcoder {config['name']}
After=network.target

[Service]
Type=simple
ExecStart={' '.join(cmd_parts)}
Restart=always
RestartSec=3
Environment=GST_DEBUG=2

[Install]
WantedBy=multi-user.target
"""

    # Write to file
    service_file = f"{SERVICE_DIR}/transcoder-{transcoder_id}.service"
    with open(service_file, "w") as f:
        f.write(service_template)
    
    return service_file

def generate_transcoder_hls_service(transcoder_id: str, config):
    """Generate systemd service file for HLS Server connected to a transcoder output"""
    try:
        # Get transcoder name and output address
        transcoder_name = config.get("name", f"transcoder-{transcoder_id}")
        output_address = config.get("output", {}).get("address", "")
        
        if not output_address:
            return False, "No output address found in transcoder configuration"
        
        stream_path = f"/var/www/html/content/{transcoder_name}"
        
        # HLS Server Service
        hls_service = f"""[Unit]
Description=HLS Server for Transcoder {transcoder_name}
After=transcoder-{transcoder_id}.service
BindsTo=transcoder-{transcoder_id}.service

[Service]
Type=simple
ExecStartPre=/bin/bash -c "mkdir -p {stream_path}"
ExecStart=/root/ristgateway/hls_server --uri={output_address} --hls-path={stream_path} --watchdog-timeout=10
ExecStopPost=/bin/bash -c "rm -f {stream_path}/*.ts {stream_path}/*.m3u8"
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
"""

        # Write HLS service file
        hls_file = f"{SERVICE_DIR}/hls-transcoder-{transcoder_id}.service"
        with open(hls_file, "w") as f:
            f.write(hls_service)
        
        return True, ""
    
    except Exception as e:
        error_msg = f"Failed to create HLS service file for transcoder: {str(e)}"
        logger.error(error_msg)
        return False, error_msg

def enable_service(service_name):
    """Enable a systemd service"""
    try:
        subprocess.run(["systemctl", "enable", service_name], check=True, capture_output=True)
        return True
    except subprocess.CalledProcessError as e:
        logger.error(f"Failed to enable service {service_name}: {e.stderr.decode()}")
        return False

def reload_systemd():
    """Reload systemd configuration"""
    try:
        subprocess.run(["systemctl", "daemon-reload"], check=True, capture_output=True)
        return True
    except subprocess.CalledProcessError as e:
        logger.error(f"Failed to reload systemd: {e.stderr.decode()}")
        return False

def get_transcoder_metrics(transcoder_id):
    """Get metrics for a transcoder by fetching from its stats server"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    transcoder = config["transcoders"][transcoder_id]
    
    # Check if the transcoder is running
    try:
        result = subprocess.run(
            ["systemctl", "is-active", f"transcoder-{transcoder_id}.service"],
            capture_output=True,
            text=True
        )
        
        is_active = result.stdout.strip() == "active"
        
        if not is_active:
            return TranscoderMetrics(
                status="stopped",
                timestamp=datetime.datetime.now().isoformat()
            ).dict()
        
        # Get metrics from the stats server
        metrics_port = transcoder.get("metrics_port", 9999)
        
        try:
            # First, get basic stats from /stats endpoint
            stats_response = requests.get(f"http://localhost:{metrics_port}/stats", timeout=2)
            if stats_response.status_code != 200:
                logger.error(f"Stats endpoint returned status {stats_response.status_code}")
                raise Exception(f"Failed to fetch stats: {stats_response.status_code}")
            
            stats_data = stats_response.json()
            
            # Then, get detailed metrics from /metrics endpoint
            metrics_response = requests.get(f"http://localhost:{metrics_port}/metrics", timeout=2)
            if metrics_response.status_code != 200:
                logger.error(f"Metrics endpoint returned status {metrics_response.status_code}")
                # If metrics endpoint fails, continue with just stats data
                metrics_data = {}
            else:
                metrics_data = metrics_response.json()
            
            # Build the complete response by combining both endpoints
            result = {
                # Basic info from /stats
                "timestamp": str(stats_data.get("timestamp_unix", "")),
                "uptime": stats_data.get("uptime_seconds", 0),
                "video_codec": stats_data.get("video_codec", "unknown"),
                "video_bitrate_kbps": stats_data.get("video_bitrate_kbps", 0),
                "audio_codec": stats_data.get("audio_codec", "unknown"),
                "audio_bitrate_kbps": stats_data.get("audio_bitrate_kbps", 0),
                "input_info": stats_data.get("input_info", {}),
                "buffer_info": stats_data.get("buffer_info", {}),
                "status": "running" if stats_data.get("pipeline_state", "") == "PLAYING" else "error"
            }
            
            # Add bitrate info - check both sources
            if "bitrates" in metrics_data:
                result["input_bitrate_mbps"] = metrics_data["bitrates"]["input_mbps"]
                result["output_bitrate_mbps"] = metrics_data["bitrates"]["output_mbps"]
                result["bitrate_history"] = metrics_data["bitrates"]["history"]
            else:
                # Fallback to stats endpoint
                result["input_bitrate_mbps"] = stats_data.get("input_bitrate_mbps", 0.0)
                result["output_bitrate_mbps"] = stats_data.get("output_bitrate_mbps", 0.0)
                result["bitrate_history"] = stats_data.get("bitrate_history", {})
            
            # Add input/output bitrate in kbps (for compatibility)
            result["input_bitrate"] = int(result["input_bitrate_mbps"] * 1000)
            result["output_bitrate"] = int(result["output_bitrate_mbps"] * 1000)
            
            # Add processing metrics from /metrics endpoint
            if "processing" in metrics_data:
                proc_data = metrics_data["processing"]
                result["processing"] = {
                    "frames_processed": proc_data.get("frames_processed", 0),
                    "frames_dropped": proc_data.get("frames_dropped", 0), 
                    "frames_delayed": proc_data.get("frames_delayed", 0),
                    "avg_qp_value": proc_data.get("avg_qp_value", 0.0),
                    "min_qp_value": proc_data.get("min_qp_value", 0.0),
                    "max_qp_value": proc_data.get("max_qp_value", 0.0),
                    "avg_encoding_time_ms": proc_data.get("avg_encoding_time_ms", 0.0),
                    "cpu_usage_percent": proc_data.get("cpu_usage_percent", 0.0),
                    "memory_usage_bytes": int(proc_data.get("memory_usage_mb", 0.0) * 1024 * 1024),
                    "video_encoding_fps": proc_data.get("encoding_fps", 0.0),
                    "end_to_end_latency_ms": proc_data.get("end_to_end_latency_ms", 0.0),
                    "pts_discontinuity": proc_data.get("pts_discontinuity", False),
                    "timestamp_gap_ns": int(proc_data.get("timestamp_gap_ms", 0.0) * 1000000),
                    "stream_info": proc_data.get("stream_info", {})
                }
                
                # Add AV sync data if available
                if "av_sync" in proc_data:
                    av_sync = proc_data["av_sync"]
                    result["processing"]["audio_video_drift_ms"] = av_sync.get("current_drift_ms", 0.0)
                    result["processing"]["last_audio_pts"] = av_sync.get("last_audio_pts", 0)
                    result["processing"]["last_video_pts"] = av_sync.get("last_video_pts", 0)
                    result["processing"]["max_drift_ms"] = av_sync.get("max_drift_ms", 0.0)
                
                # Update top-level stats too
                result["frames_processed"] = proc_data.get("frames_processed", 0)
                result["dropped_frames"] = proc_data.get("frames_dropped", 0)
                result["cpu_usage"] = proc_data.get("cpu_usage_percent", 0.0)
                result["memory_usage"] = proc_data.get("memory_usage_mb", 0.0)
            
            # Add network metrics from /metrics endpoint
            if "network" in metrics_data:
                network_data = metrics_data["network"]
                result["network"] = {
                    "input_jitter_ms": network_data.get("input_jitter_ms", 0.0),
                    "output_jitter_ms": network_data.get("output_jitter_ms", 0.0),
                    "total_input_packets": network_data.get("input_packets", 0),
                    "total_output_packets": network_data.get("output_packets", 0),
                    "dropped_packets": network_data.get("dropped_packets", 0),
                    "avg_packet_size_bytes": network_data.get("avg_packet_size_bytes", 0.0),
                    "network_stable": network_data.get("network_stable", True),
                    "reconnection_attempts": network_data.get("reconnection_attempts", 0),
                    "successful_reconnections": network_data.get("successful_reconnections", 0),
                    "last_reconnection_time": network_data.get("last_reconnection_time", 0)
                }
                
                # Also update packet counts for compatibility
                result["packets"] = {
                    "input": network_data.get("input_packets", 0),
                    "output": network_data.get("output_packets", 0)
                }
            else:
                # Use stats endpoint data as fallback
                result["packets"] = {
                    "input": stats_data.get("input_packets_total", 0),
                    "output": stats_data.get("output_packets_total", 0)
                }
            
            # Process queue data (already in the right format from /stats)
            if "buffer_info" in stats_data:
                buffer_info = stats_data["buffer_info"]
                
                # Map queue data with proper field names
                for queue_name in ["video_queue", "audio_queue", "output_queue", "audio_out_queue"]:
                    if queue_name in buffer_info:
                        queue_data = buffer_info[queue_name]
                        api_field_name = {
                            "video_queue": "input_video_queue",
                            "audio_queue": "input_audio_queue",
                            "output_queue": "output_queue",
                            "audio_out_queue": "audio_output_queue"
                        }.get(queue_name)
                        
                        if api_field_name:
                            result[api_field_name] = {
                                "current_buffers": queue_data.get("buffers", 0),
                                "current_bytes": queue_data.get("bytes", 0),
                                "current_time_ns": int(queue_data.get("time_ms", 0) * 1000000),
                                "max_buffers": 0,  # Not provided directly
                                "max_bytes": 0,    # Not provided directly
                                "max_time_ns": 0,  # Not provided directly
                                "overflow_count": 0,  # Not provided directly
                                "underflow_count": 0, # Not provided directly
                                "percent_full": queue_data.get("percent_full", 0.0)
                            }
            
            return result
            
        except requests.RequestException as e:
            logger.error(f"Error fetching stats from transcoder: {e}")
            return TranscoderMetrics(
                status="error",
                timestamp=datetime.datetime.now().isoformat()
            ).dict()
            
    except Exception as e:
        logger.error(f"Error getting transcoder metrics: {e}")
        return TranscoderMetrics(
                status="error",
                timestamp=datetime.datetime.now().isoformat()
            ).dict()

# Endpoints
@router.get("/available-devices")
def get_available_devices():
    """Return a list of available encoding devices with caching"""
    global devices_cache
    
    current_time = time.time()
    if current_time - devices_cache["timestamp"] > DEVICE_CACHE_TIMEOUT:
        # Cache is stale, refresh it
        devices = [{"id": "cpu", "name": "CPU", "type": "cpu"}]  # CPU is always available
        
        # Check for GPUs
        nvidia_gpus = detect_nvidia_gpus()
        devices.extend(nvidia_gpus)
        
        intel_devices = detect_intel_gpus()
        devices.extend(intel_devices)
        
        amd_devices = detect_amd_gpus()
        devices.extend(amd_devices)
        
        # Update cache
        devices_cache = {
            "timestamp": current_time,
            "devices": devices
        }
    
    return devices_cache["devices"]

@router.get("/next")
def get_next_transcoder_info():
    """Get information for the next transcoder to be created"""
    next_id = get_next_transcoder_id()
    next_port = get_next_transcoder_metrics_port()
    
    return {
        "transcoder_id": next_id,
        "metrics_port": next_port
    }

@router.get("")
def get_all_transcoders():
    """Get all transcoders with updated status"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    transcoders = config.get("transcoders", {})
    
    # Track if we need to save the config
    status_changed = False
    
    # Update status for each transcoder
    for transcoder_id, transcoder in transcoders.items():
        try:
            result = subprocess.run(
                ["systemctl", "is-active", f"transcoder-{transcoder_id}.service"],
                capture_output=True,
                text=True
            )
            
            actual_status = result.stdout.strip()
            new_status = ""
            
            if actual_status == "active":
                new_status = "running"
            elif actual_status == "failed":
                new_status = "error"
            else:
                new_status = "stopped"
            
            # Only update if status has changed
            if transcoder["status"] != new_status:
                transcoder["status"] = new_status
                # Also update in the config dictionary
                config["transcoders"][transcoder_id]["status"] = new_status
                status_changed = True
                
        except Exception as e:
            logger.error(f"Error checking status for {transcoder_id}: {e}")
            if transcoder["status"] != "unknown":
                transcoder["status"] = "unknown"
                config["transcoders"][transcoder_id]["status"] = "unknown"
                status_changed = True
    
    # Save config if any status changed
    if status_changed:
        try:
            save_config(config, TRANSCODER_CONFIG_FILE)
            logger.info("Updated transcoder statuses in config file")
        except Exception as e:
            logger.error(f"Failed to save updated statuses to config file: {e}")
    
    return transcoders

@router.get("/{transcoder_id}")
def get_transcoder(transcoder_id: str):
    """Get transcoder by ID"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    return config["transcoders"][transcoder_id]

@router.post("/{transcoder_id}")
def create_transcoder(transcoder_id: str, transcoder: TranscoderChannel):
    """Create a new transcoder"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    
    # Check if transcoder already exists
    if transcoder_id in config.get("transcoders", {}):
        raise HTTPException(status_code=400, detail="Transcoder ID already exists")
    
    # Convert pydantic model to dict
    transcoder_dict = transcoder.dict()
    
    # Assign metrics port if not provided
    if not transcoder_dict.get("metrics_port"):
        transcoder_dict["metrics_port"] = get_next_transcoder_metrics_port()
    
    # Ensure transcoders key exists
    if "transcoders" not in config:
        config["transcoders"] = {}
    
    # Add transcoder to config
    config["transcoders"][transcoder_id] = transcoder_dict
    
    # Save config
    save_config(config, TRANSCODER_CONFIG_FILE)
    
    # Generate service file
    try:
        generate_transcoder_service_file(transcoder_id, transcoder_dict)
        
        # Generate HLS service file
        generate_transcoder_hls_service(transcoder_id, transcoder_dict)
        
        reload_systemd()
    except Exception as e:
        logger.error(f"Error generating service file: {e}")
        # Continue anyway since the transcoder is created in the config
    
    return transcoder_dict

@router.put("/{transcoder_id}")
def update_transcoder(transcoder_id: str, transcoder: TranscoderChannel):
    """Update an existing transcoder"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    
    # Check if transcoder exists
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    # Convert pydantic model to dict
    transcoder_dict = transcoder.dict()
    
    # Preserve metrics port if not provided
    if not transcoder_dict.get("metrics_port") and config["transcoders"][transcoder_id].get("metrics_port"):
        transcoder_dict["metrics_port"] = config["transcoders"][transcoder_id]["metrics_port"]
    
    # Update transcoder in config
    config["transcoders"][transcoder_id] = transcoder_dict
    
    # Save config
    save_config(config, TRANSCODER_CONFIG_FILE)
    
    # Re-generate service file
    try:
        generate_transcoder_service_file(transcoder_id, transcoder_dict)
        
        # Re-generate HLS service file
        generate_transcoder_hls_service(transcoder_id, transcoder_dict)
        
        reload_systemd()
        
        # Restart the service if it's running
        current_status = subprocess.run(
            ["systemctl", "is-active", f"transcoder-{transcoder_id}.service"],
            capture_output=True,
            text=True
        ).stdout.strip()
        
        if current_status == "active":
            subprocess.run(["systemctl", "restart", f"transcoder-{transcoder_id}.service"], check=True)
            # Also restart the HLS service
            subprocess.run(["systemctl", "restart", f"hls-transcoder-{transcoder_id}.service"], check=True)
    except Exception as e:
        logger.error(f"Error updating service file: {e}")
    
    return transcoder_dict

@router.delete("/{transcoder_id}")
def delete_transcoder(transcoder_id: str):
    """Delete a transcoder"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    
    # Check if transcoder exists
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    # Stop the service if it's running
    try:
        subprocess.run(["systemctl", "stop", f"transcoder-{transcoder_id}.service"], check=False)
        subprocess.run(["systemctl", "stop", f"hls-transcoder-{transcoder_id}.service"], check=False)
        subprocess.run(["systemctl", "disable", f"transcoder-{transcoder_id}.service"], check=False)
        subprocess.run(["systemctl", "disable", f"hls-transcoder-{transcoder_id}.service"], check=False)
    except:
        pass
    
    # Remove service files
    service_file = f"{SERVICE_DIR}/transcoder-{transcoder_id}.service"
    hls_file = f"{SERVICE_DIR}/hls-transcoder-{transcoder_id}.service"
    
    if os.path.exists(service_file):
        os.remove(service_file)
    
    if os.path.exists(hls_file):
        os.remove(hls_file)
    
    # Remove from config
    del config["transcoders"][transcoder_id]
    
    # Save config
    save_config(config, TRANSCODER_CONFIG_FILE)
    
    # Reload systemd
    reload_systemd()
    
    return {"status": "deleted", "transcoder_id": transcoder_id}

@router.put("/{transcoder_id}/start")
def start_transcoder(transcoder_id: str):
    """Start a transcoder and its associated HLS service"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    
    # Check if transcoder exists
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    # Start the services
    try:
        # Start transcoder first
        subprocess.run(["systemctl", "start", f"transcoder-{transcoder_id}.service"], check=True)
        
        # Wait a brief moment for the transcoder to initialize
        time.sleep(1)
        
        # Explicitly start the HLS service 
        try:
            subprocess.run(["systemctl", "start", f"hls-transcoder-{transcoder_id}.service"], check=True)
            logger.info(f"Started HLS service for transcoder {transcoder_id}")
        except Exception as e:
            logger.warning(f"Failed to start HLS service for transcoder {transcoder_id}: {e}")
        
        # Update status in config
        config["transcoders"][transcoder_id]["status"] = "running"
        save_config(config, TRANSCODER_CONFIG_FILE)
        
        return {"status": "started", "transcoder_id": transcoder_id, "hls": "started"}
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Failed to start transcoder: {e.stderr.decode() if hasattr(e, 'stderr') else str(e)}")

@router.put("/{transcoder_id}/stop")
def stop_transcoder(transcoder_id: str):
    """Stop a transcoder"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    
    # Check if transcoder exists
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    # Stop the service
    try:
        subprocess.run(["systemctl", "stop", f"transcoder-{transcoder_id}.service"], check=True)
        
        # Update status in config
        config["transcoders"][transcoder_id]["status"] = "stopped"
        save_config(config, TRANSCODER_CONFIG_FILE)
        
        return {"status": "stopped", "transcoder_id": transcoder_id}
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Failed to stop transcoder: {e.stderr.decode() if hasattr(e, 'stderr') else str(e)}")

@router.get("/{transcoder_id}/metrics")
def get_metrics(transcoder_id: str):
    """Get metrics for a transcoder"""
    metrics = get_transcoder_metrics(transcoder_id)
    return metrics

@router.get("/{transcoder_id}/buffers")
def get_buffer_stats(transcoder_id: str):
    """Get buffer statistics for a transcoder"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    transcoder = config["transcoders"][transcoder_id]
    metrics_port = transcoder.get("metrics_port", 9999)
    
    try:
        response = requests.get(f"http://localhost:{metrics_port}/buffers", timeout=2)
        
        if response.status_code != 200:
            raise HTTPException(status_code=502, detail="Failed to fetch buffer stats")
        
        return response.json()
        
    except requests.RequestException as e:
        logger.error(f"Error fetching buffer stats: {e}")
        raise HTTPException(status_code=502, detail="Failed to connect to transcoder stats server")

@router.get("/{transcoder_id}/metrics/processing")
def get_processing_metrics(transcoder_id: str):
    """Get processing metrics for a transcoder"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    transcoder = config["transcoders"][transcoder_id]
    metrics_port = transcoder.get("metrics_port", 9999)
    
    try:
        response = requests.get(f"http://localhost:{metrics_port}/metrics", timeout=2)
        
        if response.status_code != 200:
            raise HTTPException(status_code=502, detail="Failed to fetch processing metrics")
        
        return response.json()
        
    except requests.RequestException as e:
        logger.error(f"Error fetching processing metrics: {e}")
        raise HTTPException(status_code=502, detail="Failed to connect to transcoder stats server")

@router.get("/gstreamer/plugins")
def get_gstreamer_plugins():
    """Get list of available GStreamer plugins"""
    try:
        result = subprocess.run(
            ["gst-inspect-1.0"], 
            capture_output=True, 
            text=True,
            check=False
        )
        
        if result.returncode != 0:
            raise HTTPException(status_code=500, detail="Failed to retrieve GStreamer plugins")
        
        plugins = []
        lines = result.stdout.strip().split('\n')
        
        for line in lines:
            if ': ' in line and not line.startswith(' '):
                parts = line.split(': ', 1)
                if len(parts) == 2:
                    name, description = parts
                    plugins.append({"name": name.strip(), "description": description.strip()})
        
        return plugins
    except Exception as e:
        logger.error(f"Error getting GStreamer plugins: {e}")
        raise HTTPException(status_code=500, detail=f"Error retrieving plugins: {str(e)}")

@router.get("/status")
def get_transcoders_status():
    """Get status summary of all transcoders"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    
    status_summary = {
        "total": 0,
        "running": 0,
        "stopped": 0,
        "error": 0,
        "transcoders": []
    }
    
    for transcoder_id, transcoder in config.get("transcoders", {}).items():
        # Get actual status from systemd
        try:
            result = subprocess.run(
                ["systemctl", "is-active", f"transcoder-{transcoder_id}.service"],
                capture_output=True,
                text=True
            )
            
            actual_status = result.stdout.strip()
            if actual_status == "active":
                status = "running"
            elif actual_status == "failed":
                status = "error"
            else:
                status = "stopped"
            
            # Update config if status has changed
            if transcoder.get("status") != status:
                transcoder["status"] = status
                config["transcoders"][transcoder_id]["status"] = status
            
            # Add to summary
            status_summary["total"] += 1
            status_summary[status] += 1
            
            status_summary["transcoders"].append({
                "id": transcoder_id,
                "name": transcoder.get("name", ""),
                "status": status
            })
            
        except Exception as e:
            logger.error(f"Error checking status for {transcoder_id}: {e}")
    
    # Save updated statuses
    save_config(config, TRANSCODER_CONFIG_FILE)
    
    return status_summary
