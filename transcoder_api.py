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
    program_pid: Optional[int] = None
    video_pid: Optional[int] = None
    audio_pid: Optional[int] = None

class VideoConfig(BaseModel):
    device: str = "cpu"
    codec: str
    resolution: str
    b_frames: int = 0
    profile: str
    preset: str
    deinterlace: bool = False
    bitrate: int  # kbps

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

class TranscoderChannel(BaseModel):
    name: str
    input: TranscoderInput
    video: VideoConfig
    audio: AudioConfig
    output: TranscoderOutput
    enabled: bool = True
    status: str = "stopped"
    metrics_port: Optional[int] = None

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
    
    # Basic service template
    service_template = f"""[Unit]
Description=Binary Transcoder {config['name']}
After=network.target

[Service]
Type=simple
ExecStart=/root/ristgateway/basic_transcoder --input-uri {input_address} --output-uri {output_address} --video-codec {video_codec} --video-bitrate {video_bitrate} --audio-codec {audio_codec} --audio-bitrate {audio_bitrate} --stats-port {metrics_port}
Restart=always
RestartSec=3
Environment=GST_DEBUG=3

[Install]
WantedBy=multi-user.target
"""

    # Write to file
    service_file = f"{SERVICE_DIR}/transcoder-{transcoder_id}.service"
    with open(service_file, "w") as f:
        f.write(service_template)
    
    return service_file

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
            return {
                "cpu_usage": 0.0,
                "memory_usage": 0.0,
                "output_bitrate": 0,
                "input_bitrate": 0,
                "frames_processed": 0,
                "dropped_frames": 0,
                "status": "stopped",
                "uptime": 0,
                "timestamp": datetime.datetime.now().isoformat()
            }
        
        # Get metrics from the stats server
        metrics_port = transcoder.get("metrics_port", 9999)  # Default port if not specified
        
        try:
            response = requests.get(f"http://localhost:{metrics_port}/stats", timeout=2)
            
            if response.status_code != 200:
                logger.error(f"Metrics endpoint returned status {response.status_code}")
                raise Exception(f"Failed to fetch metrics: {response.status_code}")
            
            # Parse JSON response from the binary transcoder
            metrics_data = response.json()
            
            # Map the data to our existing format
            return {
                "cpu_usage": 0.0,  # Not provided by the transcoder yet
                "memory_usage": 0.0,  # Not provided by the transcoder yet
                "input_bitrate": metrics_data.get("input_bitrate_bps", 0) / 1000,  # Convert to kbps
                "output_bitrate": metrics_data.get("output_bitrate_bps", 0) / 1000,  # Convert to kbps
                "input_bitrate_mbps": metrics_data.get("input_bitrate_mbps", 0),
                "output_bitrate_mbps": metrics_data.get("output_bitrate_mbps", 0),
                "frames_processed": 0,  # Not provided by the transcoder yet
                "dropped_frames": 0,  # Not provided by the transcoder yet
                "status": "running" if metrics_data.get("pipeline_state") == "PLAYING" else "error",
                "uptime": metrics_data.get("uptime_seconds", 0),
                "timestamp": datetime.datetime.now().isoformat(),
                "video_codec": metrics_data.get("video_codec", "unknown"),
                "video_bitrate_kbps": metrics_data.get("video_bitrate_kbps", 0),
                "audio_codec": metrics_data.get("audio_codec", "unknown"),
                "audio_bitrate_kbps": metrics_data.get("audio_bitrate_kbps", 0),
                "packets": {
                    "input": metrics_data.get("input_packets_total", 0),
                    "output": metrics_data.get("output_packets_total", 0)
                }
            }
            
        except requests.RequestException as e:
            logger.error(f"Error fetching stats from transcoder: {e}")
            
            # Return basic info even if metrics fetch failed
            return {
                "cpu_usage": 0.0,
                "memory_usage": 0.0,
                "output_bitrate": 0,
                "input_bitrate": 0,
                "status": "error",
                "uptime": 0,
                "error": str(e),
                "timestamp": datetime.datetime.now().isoformat()
            }
            
    except Exception as e:
        logger.error(f"Error getting transcoder metrics: {e}")
        return {
            "cpu_usage": 0.0,
            "memory_usage": 0.0,
            "output_bitrate": 0,
            "input_bitrate": 0,
            "frames_processed": 0,
            "dropped_frames": 0,
            "status": "error",
            "uptime": 0,
            "error": str(e),
            "timestamp": datetime.datetime.now().isoformat()
        }

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
        reload_systemd()
        
        # Restart the service if it's running
        current_status = subprocess.run(
            ["systemctl", "is-active", f"transcoder-{transcoder_id}.service"],
            capture_output=True,
            text=True
        ).stdout.strip()
        
        if current_status == "active":
            subprocess.run(["systemctl", "restart", f"transcoder-{transcoder_id}.service"], check=True)
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
        subprocess.run(["systemctl", "disable", f"transcoder-{transcoder_id}.service"], check=False)
    except:
        pass
    
    # Remove service file
    service_file = f"{SERVICE_DIR}/transcoder-{transcoder_id}.service"
    if os.path.exists(service_file):
        os.remove(service_file)
    
    # Remove from config
    del config["transcoders"][transcoder_id]
    
    # Save config
    save_config(config, TRANSCODER_CONFIG_FILE)
    
    # Reload systemd
    reload_systemd()
    
    return {"status": "deleted", "transcoder_id": transcoder_id}

@router.put("/{transcoder_id}/start")
def start_transcoder(transcoder_id: str):
    """Start a transcoder"""
    config = load_config(TRANSCODER_CONFIG_FILE)
    
    # Check if transcoder exists
    if transcoder_id not in config.get("transcoders", {}):
        raise HTTPException(status_code=404, detail="Transcoder not found")
    
    # Start the service
    try:
        subprocess.run(["systemctl", "start", f"transcoder-{transcoder_id}.service"], check=True)
        
        # Update status in config
        config["transcoders"][transcoder_id]["status"] = "running"
        save_config(config, TRANSCODER_CONFIG_FILE)
        
        return {"status": "started", "transcoder_id": transcoder_id}
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