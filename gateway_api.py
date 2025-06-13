from fastapi import FastAPI, HTTPException, Query, Body
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
import yaml
import os
import subprocess
import requests
import json
import time
from datetime import datetime
from typing import Dict, Optional, List
import logging
import traceback

network_stats_history = {}


# Import the transcoder router
from transcoder_api import router as transcoder_router

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler("/var/log/rist-api.log"),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

app = FastAPI(title="RIST Manager API")

# Constants
SERVICE_DIR = "/etc/systemd/system"

# Add CORS middleware to allow cross-origin requests
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Allow all origins
    allow_credentials=True,
    allow_methods=["*"],  # Allow all methods
    allow_headers=["*"],  # Allow all headers
)

# Metrics Caching
metrics_cache = {}

# Config file paths
RECEIVER_CONFIG_FILE = "receiver_config.yaml"
SENDER_CONFIG_FILE = "sender_config.yaml"
BACKUP_SOURCES_FILE = "backup_sources.yaml"

# Channel base model
class ChannelBase(BaseModel):
    name: str
    enabled: bool = True
    metrics_port: Optional[int] = None
    status: str = "stopped"

# Sender channel model
class SenderChannel(ChannelBase):
    input_source: str  # Local source (e.g., "udp://127.0.0.1:10000")
    destination_ip: str
    destination_port: int
    virt_src_port: Optional[int] = None
    buffer: int = Field(1000, description="Buffer size in ms")
    encryption_type: Optional[int] = None
    secret: Optional[str] = None

# Receiver channel model
class ReceiverChannel(ChannelBase):
    source_ip: str
    source_port: int
    virt_dst_port: Optional[int] = None
    multicast_ip: str = "224.2.2.2"
    multicast_port: int = 10000
    buffer: int = Field(1000, description="Buffer size in ms")
    encryption_type: Optional[int] = None
    secret: Optional[str] = None
    backup_sources: List[str] = []

# Define the RistReceiverSettings model
class RistReceiverSettings(BaseModel):
    profile: int = Field(1, description="RIST profile")
    virt_src_port: Optional[int] = None
    buffer: int = Field(1000, description="Buffer size in ms")
    encryption_type: Optional[int] = None
    secret: Optional[str] = None

class ReceiverChannelUpdate(BaseModel):
    name: str
    enabled: bool = True
    input: str
    output: str
    settings: RistReceiverSettings
    metrics_port: int

class SenderChannelUpdate(BaseModel):
    name: str
    enabled: bool = True
    input: str
    output: str
    settings: RistReceiverSettings
    metrics_port: int

# Helper Functions
def load_config(file_path):
    """Load configuration from YAML file"""
    if not os.path.exists(file_path):
        return {"channels": {}}
    with open(file_path, 'r') as f:
        return yaml.safe_load(f) or {"channels": {}}

def save_config(config, file_path):
    """Save configuration to YAML file"""
    # Ensure directory exists
    os.makedirs(os.path.dirname(file_path) if os.path.dirname(file_path) else '.', exist_ok=True)
    with open(file_path, 'w') as f:
        yaml.dump(config, f)

def get_next_metrics_port():
    """Find the next available metrics port across both configurations"""
    max_port = 9200  # Starting port number
    
    # Check receiver config
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    for channel in receiver_config.get("channels", {}).values():
        if channel.get("metrics_port", 0) > max_port:
            max_port = channel["metrics_port"]
    
    # Check sender config
    sender_config = load_config(SENDER_CONFIG_FILE)
    for channel in sender_config.get("channels", {}).values():
        if channel.get("metrics_port", 0) > max_port:
            max_port = channel["metrics_port"]
    
    return max_port + 1


def parse_prometheus_metrics2(metrics_text: str) -> dict:
    metrics = {
        "peers": [],
        "quality": 100.0,
        "total_bandwidth_bps": 0,
        "total_retry_bandwidth_bps": 0,
        "packets": {
            "sent": 0,
            "received": 0,
            "missing": 0,
            "reordered": 0,
            "recovered": 0,
            "recovered_one_retry": 0,
            "lost": 0
        },
        "timing": {
            "min_iat": 0,
            "cur_iat": 0,
            "max_iat": 0,
            "rtt": 0
        }
    }

    try:
        for line in metrics_text.split('\n'):
            if line.startswith('#') or not line.strip():
                continue
            
            if '{' in line:
                name = line.split('{')[0].strip()
                labels_str = line.split('{')[1].split('}')[0]
                value = float(line.split('}')[1].strip())

                # Parse labels
                labels = {}
                for label in labels_str.split(','):
                    key, val = label.strip().split('=')
                    labels[key] = val.strip('"')

                # Create peer info structure
                peer_info = {
                    "id": labels.get('peer_id', labels.get('flow_id', 'unknown')),
                    "cname": labels.get('cname', 'unknown'),
                    "listening": labels.get('listening', 'unknown'),
                    "peer_url": labels.get('peer_url', 'N/A'),
                    "bandwidth_bps": 0,
                    "retry_bandwidth_bps": 0,
                    "packets": {
                        "sent": 0,
                        "received": 0,
                        "retransmitted": 0,
                        "rtt": 0,
                        "quality": 100.0
                    }
                }

                # Find or create peer
                peer = next((p for p in metrics['peers'] if p['id'] == peer_info['id']), None)
                if not peer:
                    metrics['peers'].append(peer_info)
                    peer = peer_info

                # Update metrics based on metric name
                if "rist_sender_peer_quality" in name:
                    peer['packets']['quality'] = value
                    metrics['quality'] = value
                elif "rist_sender_peer_bandwidth_bps" in name:
                    peer['bandwidth_bps'] = value
                    metrics['total_bandwidth_bps'] += value
                elif "rist_sender_peer_retry_bandwidth_bps" in name:
                    peer['retry_bandwidth_bps'] = value
                    metrics['total_retry_bandwidth_bps'] += value
                elif "rist_sender_peer_sent_packets" in name:
                    peer['packets']['sent'] = int(value)
                    metrics['packets']['sent'] += int(value)
                elif "rist_sender_peer_received_packets" in name:
                    peer['packets']['received'] = int(value)
                    metrics['packets']['received'] += int(value)
                elif "rist_sender_peer_retransmitted_packets" in name:
                    peer['packets']['retransmitted'] = int(value)
                    metrics['packets']['recovered'] += int(value)
                elif "rist_sender_peer_rtt_seconds" in name:
                    peer['packets']['rtt'] = value
                    metrics['timing']['rtt'] = value
                elif "rist_sender_peer_min_iat_seconds" in name:
                    metrics['timing']['min_iat'] = value
                elif "rist_sender_peer_cur_iat_seconds" in name:
                    metrics['timing']['cur_iat'] = value
                elif "rist_sender_peer_max_iat_seconds" in name:
                    metrics['timing']['max_iat'] = value

    except Exception as e:
        logger.error(f"Error parsing metrics2: {e}")
    
    return metrics

def get_cached_metrics2(channel_id: str, metrics_port: int, cache_timeout: int = 1):
    current_time = time.time()
    
    if (channel_id in metrics_cache and 
        current_time - metrics_cache[channel_id]['timestamp'] < cache_timeout):
        return metrics_cache[channel_id]['data']
    
    try:
        logger.info(f"Fetching metrics2 for channel {channel_id} from port {metrics_port}")
        response = requests.get(f"http://localhost:{metrics_port}/metrics", timeout=2)
        
        if response.status_code != 200:
            logger.error(f"Metrics endpoint returned status {response.status_code}")
            return parse_prometheus_metrics2("")
        
        metrics = parse_prometheus_metrics2(response.text)
        
        metrics_cache[channel_id] = {
            'timestamp': current_time,
            'data': metrics
        }
        
        return metrics
        
    except requests.RequestException as e:
        logger.error(f"Error fetching metrics for channel {channel_id}: {e}")
        return parse_prometheus_metrics("")



def parse_prometheus_metrics(metrics_text: str) -> dict:
    """Parse Prometheus metrics text into structured data"""
    metrics = {
        "quality": 100.0,
        "peers": 0,
        "bandwidth_bps": 0,
        "retry_bandwidth_bps": 0,
        "packets": {
            "sent": 0,
            "received": 0,
            "missing": 0,
            "reordered": 0,
            "recovered": 0,
            "recovered_one_retry": 0,
            "lost": 0
        },
        "timing": {
            "min_iat": 0,
            "cur_iat": 0,
            "max_iat": 0,
            "rtt": 0
        }
    }
    
    if isinstance(metrics_text, str):
        metrics_text = metrics_text.replace('\\n', '\n').replace('\\"', '"')
    
    try:
        for line in metrics_text.split('\n'):
            if line.startswith('#') or not line.strip():
                continue
                
            if '{' in line:
                name, rest = line.split('{', 1)
                name = name.strip()
                labels, value_part = rest.rsplit('}', 1)
                value = float(value_part.strip())
                
                if name == 'rist_client_flow_peers':
                    metrics['peers'] = int(value)
                elif name == 'rist_client_flow_bandwidth_bps':
                    metrics['bandwidth_bps'] = value
                elif name == 'rist_client_flow_retry_bandwidth_bps':
                    metrics['retry_bandwidth_bps'] = value
                elif name == 'rist_client_flow_sent_packets_total':
                    metrics['packets']['sent'] = int(value)
                elif name == 'rist_client_flow_received_packets_total':
                    metrics['packets']['received'] = int(value)
                elif name == 'rist_client_flow_missing_packets_total':
                    metrics['packets']['missing'] = int(value)
                elif name == 'rist_client_flow_reordered_packets_total':
                    metrics['packets']['reordered'] = int(value)
                elif name == 'rist_client_flow_recovered_packets_total':
                    metrics['packets']['recovered'] = int(value)
                elif name == 'rist_client_flow_recovered_one_retry_packets_total':
                    metrics['packets']['recovered_one_retry'] = int(value)
                elif name == 'rist_client_flow_lost_packets_total':
                    metrics['packets']['lost'] = int(value)
                elif name == 'rist_client_flow_min_iat_seconds':
                    metrics['timing']['min_iat'] = value
                elif name == 'rist_client_flow_cur_iat_seconds':
                    metrics['timing']['cur_iat'] = value
                elif name == 'rist_client_flow_max_iat_seconds':
                    metrics['timing']['max_iat'] = value
                elif name == 'rist_client_flow_rtt_seconds':
                    metrics['timing']['rtt'] = value
                elif name == 'rist_client_flow_quality':
                    metrics['quality'] = value

    except Exception as e:
        logger.error(f"Error parsing metrics: {e}")
        logger.error(f"Metrics text: {metrics_text}")
    
    return metrics


def get_cached_metrics(channel_id: str, metrics_port: int, cache_timeout: int = 1):
    """
    Retrieve metrics with a simple caching mechanism
    """
    current_time = time.time()
    
    # Check if we have a cached result and it's fresh
    if (channel_id in metrics_cache and 
        current_time - metrics_cache[channel_id]['timestamp'] < cache_timeout):
        return metrics_cache[channel_id]['data']
    
    # Fetch new metrics
    try:
        logger.info(f"Fetching metrics for channel {channel_id} from port {metrics_port}")
        response = requests.get(f"http://localhost:{metrics_port}/metrics", timeout=2)
        
        if response.status_code != 200:
            logger.error(f"Metrics endpoint returned status {response.status_code}")
            return parse_prometheus_metrics("")
        
        metrics = parse_prometheus_metrics(response.text)
        
        # Cache the result
        metrics_cache[channel_id] = {
            'timestamp': current_time,
            'data': metrics
        }
        
        return metrics
    
    except requests.RequestException as e:
        logger.error(f"Error fetching metrics for channel {channel_id}: {e}")
        return parse_prometheus_metrics("")


def reload_systemd():
    """Reload systemd configuration"""
    try:
        subprocess.run(["systemctl", "daemon-reload"], check=True, capture_output=True)
        return True
    except subprocess.CalledProcessError as e:
        logger.error(f"Failed to reload systemd: {e.stderr.decode()}")
        return False

def generate_receiver_service_file(channel_id: str, channel: Dict):
    """Generate systemd service files for RIST receiver and HLS Server"""
    try:
        # Parse RIST input URL
        input_url = channel["input"]
        # Add virt-dst-port to URL if specified and not already in URL
        if channel["settings"].get("virt_src_port") and "virt-dst-port" not in input_url:
            input_url += f"?virt-dst-port={channel['settings']['virt_src_port']}"

        metrics_port = channel['metrics_port']
        log_port = metrics_port + 1000
        stream_path = f"/var/www/html/content/{channel['name']}"
        
        # RIST receiver service components
        rist_cmd_parts = [
            "/usr/local/bin/ristreceiver",
            f"-p {channel['settings']['profile']}",
            f"-i '{input_url}'",
            f"-o '{channel['output']}'",
            "-v 6",
            "-M",
            "--metrics-http",
            f"--metrics-port={metrics_port}",
            "-S 1000",
            f"-r localhost:{log_port}"
        ]

        # Add optional parameters
        if channel["settings"].get("buffer"):
            rist_cmd_parts.append(f"-b {channel['settings']['buffer']}")
        
        if channel["settings"].get("encryption_type"):
            rist_cmd_parts.append(f"-e {channel['settings']['encryption_type']}")
        
        if channel["settings"].get("secret"):
            rist_cmd_parts.append(f"-s {channel['settings']['secret']}")

        # RIST Receiver Service
        rist_service = f"""[Unit]
Description=RIST Receiver Channel {channel['name']}
After=network.target

[Service]
Type=simple
ExecStart={' '.join(rist_cmd_parts)}
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
"""

        # HLS Server Service (replacing FFmpeg)
        hls_service = f"""[Unit]
Description=HLS Server for Receiver Channel {channel['name']}
After=rist-receiver-{channel_id}.service
BindsTo=rist-receiver-{channel_id}.service

[Service]
Type=simple
ExecStartPre=/bin/bash -c "mkdir -p {stream_path}"
ExecStart=/root/ristgateway/hls_server --uri={channel['output']} --hls-path={stream_path} --watchdog-timeout=10
ExecStopPost=/bin/bash -c "rm -f {stream_path}/*.ts {stream_path}/*.m3u8"
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
"""

        # Create log directory if it doesn't exist
        os.makedirs("/var/log/ristreceiver", exist_ok=True)

        # Write RIST service file
        rist_file = f"{SERVICE_DIR}/rist-receiver-{channel_id}.service"
        with open(rist_file, "w") as f:
            f.write(rist_service)
        
        # Write HLS service file
        hls_file = f"{SERVICE_DIR}/hls-receiver-{channel_id}.service"
        with open(hls_file, "w") as f:
            f.write(hls_service)
        
        return True, ""
    
    except Exception as e:
        error_msg = f"Failed to create service files: {str(e)}\n{traceback.format_exc()}"
        logger.error(error_msg)
        return False, error_msg

def generate_sender_service_file(channel_id: str, channel: Dict):
    """Generate systemd service files for RIST sender and HLS Server"""
    try:
        # Parse RIST output URL
        output_url = channel["output"]
        # Add virt-src-port to URL if specified and not already in URL
        if channel["settings"].get("virt_src_port") and "virt-dst-port" not in output_url:
            output_url += f"?virt-dst-port={channel['settings']['virt_src_port']}"

        metrics_port = channel['metrics_port']
        log_port = metrics_port + 1000
        stream_path = f"/var/www/html/content/{channel['name']}"
        
        # RIST sender service components
        rist_cmd_parts = [
            "/usr/local/bin/ristsender",
            f"-p {channel['settings']['profile']}",
            f"-i '{channel['input']}'",
            f"-o '{output_url}'",
            "-v 6",
            "-M",
            "--metrics-http",
            f"--metrics-port={metrics_port}",
            "-S 1000",
            f"-r localhost:{log_port}"
        ]

        # Add optional parameters
        if channel["settings"].get("buffer"):
            rist_cmd_parts.append(f"-b {channel['settings']['buffer']}")
        
        if channel["settings"].get("encryption_type"):
            rist_cmd_parts.append(f"-e {channel['settings']['encryption_type']}")
        
        if channel["settings"].get("secret"):
            rist_cmd_parts.append(f"-s {channel['settings']['secret']}")

        # RIST Sender Service
        rist_service = f"""[Unit]
Description=RIST Sender Channel {channel['name']}
After=network.target

[Service]
Type=simple
ExecStart={' '.join(rist_cmd_parts)}
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
"""

        # HLS Server Service (replacing FFmpeg)
        hls_service = f"""[Unit]
Description=HLS Server for Sender Channel {channel['name']}
After=rist-sender-{channel_id}.service
BindsTo=rist-sender-{channel_id}.service

[Service]
Type=simple
ExecStartPre=/bin/bash -c "mkdir -p {stream_path}"
ExecStart=/root/ristgateway/hls_server --uri={channel['input']} --hls-path={stream_path} --watchdog-timeout=10
ExecStopPost=/bin/bash -c "rm -f {stream_path}/*.ts {stream_path}/*.m3u8"
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
"""

        # Create log directory if it doesn't exist
        os.makedirs("/var/log/ristreceiver", exist_ok=True)

        # Write RIST service file
        rist_file = f"{SERVICE_DIR}/rist-sender-{channel_id}.service"
        with open(rist_file, "w") as f:
            f.write(rist_service)
        
        # Write HLS service file
        hls_file = f"{SERVICE_DIR}/hls-sender-{channel_id}.service"
        with open(hls_file, "w") as f:
            f.write(hls_service)
        
        return True, ""
    
    except Exception as e:
        error_msg = f"Failed to create service files: {str(e)}\n{traceback.format_exc()}"
        logger.error(error_msg)
        return False, error_msg

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
        error_msg = f"Failed to create HLS service file for transcoder: {str(e)}\n{traceback.format_exc()}"
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

def disable_service(service_name):
    """Disable a systemd service"""
    try:
        subprocess.run(["systemctl", "disable", service_name], check=True, capture_output=True)
        return True
    except subprocess.CalledProcessError as e:
        logger.error(f"Failed to disable service {service_name}: {e.stderr.decode()}")
        return False

# API Endpoints

# Include the transcoder router
app.include_router(transcoder_router)


@app.get("/health")
def health_check():
    """Simple health check endpoint"""
    return {"status": "healthy"}

@app.get("/channels")
def get_channels():
    """Retrieve all channels from both configurations"""
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    sender_config = load_config(SENDER_CONFIG_FILE)
    
    # Function to check service status
    def check_service_status(channel_id, is_receive=True):
        try:
            service_prefix = "rist-receiver" if is_receive else "rist-sender"
            service_name = f"{service_prefix}-{channel_id}.service"
            
            # Run systemctl to check service status
            result = subprocess.run(
                ["systemctl", "is-active", service_name], 
                capture_output=True, 
                text=True
            )
            
            # Determine status based on systemctl output
            if result.returncode == 0:
                return "running"
            else:
                return "stopped"
        except Exception as e:
            logger.error(f"Error checking status for {service_name}: {e}")
            return "unknown"
    
    # Update receiver channels
    for channel_id, channel in receiver_config.get("channels", {}).items():
        channel["channel_type"] = "receive"
        # Update status based on actual service state
        channel["status"] = check_service_status(channel_id, is_receive=True)
    
    # Update sender channels
    for channel_id, channel in sender_config.get("channels", {}).items():
        channel["channel_type"] = "send"
        # Update status based on actual service state
        channel["status"] = check_service_status(channel_id, is_receive=False)
    
    # Combine channels from both configurations
    all_channels = {**receiver_config.get("channels", {}), **sender_config.get("channels", {})}
    return all_channels

@app.get("/channels/active")
async def get_active_channels():
    """Get all active channels with their metrics for the dashboard"""
    try:
        # Load both receiver and sender configs
        receiver_config = load_config(RECEIVER_CONFIG_FILE)
        sender_config = load_config(SENDER_CONFIG_FILE)
        
        active_channels = {}
        
        # Process receiver channels
        for channel_id, channel in receiver_config.get("channels", {}).items():
            # Check if channel is running
            service_name = f"rist-receiver-{channel_id}.service"
            result = subprocess.run(
                ["systemctl", "is-active", service_name],
                capture_output=True,
                text=True
            )
            
            if result.stdout.strip() != "active":
                continue  # Skip non-active channels
            
            # Channel is active, get metrics
            try:
                metrics_port = channel.get("metrics_port")
                if not metrics_port:
                    continue
                    
                metrics = get_cached_metrics(channel_id, metrics_port)
                
                # Add to active channels
                active_channels[channel_id] = {
                    "name": channel.get("name", f"Channel {channel_id}"),
                    "status": "running",
                    "input": {
                        "url": channel.get("input", "unknown"),
                        "quality": metrics.get("quality", 100.0),
                        "total_bandwidth_bps": metrics.get("bandwidth_bps", 0),
                        "peers": metrics.get("peers", 0)
                    },
                    "output": {
                        "url": channel.get("output", "unknown"),
                        "quality": 100.0,  # Receiver output quality is typically 100%
                        "total_bandwidth_bps": metrics.get("bandwidth_bps", 0),
                        "peers": 1
                    }
                }
            except Exception as e:
                logger.error(f"Error getting metrics for receiver {channel_id}: {str(e)}")
        
        # Process sender channels
        for channel_id, channel in sender_config.get("channels", {}).items():
            # Check if channel is running
            service_name = f"rist-sender-{channel_id}.service"
            result = subprocess.run(
                ["systemctl", "is-active", service_name],
                capture_output=True,
                text=True
            )
            
            if result.stdout.strip() != "active":
                continue  # Skip non-active channels
            
            # Channel is active, get metrics
            try:
                metrics_port = channel.get("metrics_port")
                if not metrics_port:
                    continue
                    
                metrics2 = get_cached_metrics2(channel_id, metrics_port)
                
                # Add to active channels
                active_channels[channel_id] = {
                    "name": channel.get("name", f"Channel {channel_id}"),
                    "status": "running",
                    "input": {
                        "url": channel.get("input", "unknown"),
                        "quality": 100.0,  # Sender input quality is typically 100%
                        "total_bandwidth_bps": metrics2.get("total_bandwidth_bps", 0),
                        "peers": len(metrics2.get("peers", []))
                    },
                    "output": {
                        "url": channel.get("output", "unknown"),
                        "quality": metrics2.get("quality", 100.0),
                        "total_bandwidth_bps": metrics2.get("total_bandwidth_bps", 0),
                        "peers": len(metrics2.get("peers", []))
                    }
                }
            except Exception as e:
                logger.error(f"Error getting metrics for sender {channel_id}: {str(e)}")
        
        return active_channels
        
    except Exception as e:
        logger.error(f"Error in get_active_channels: {str(e)}")
        logger.error(traceback.format_exc())
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/channels/next")
def get_next_channel_info():
    """Get information for the next channel to be created"""
    next_metrics_port = get_next_metrics_port()
    
    # Find the highest channel number across both configs
    max_num = 0
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    sender_config = load_config(SENDER_CONFIG_FILE)
    
    for channel_id in list(receiver_config.get("channels", {}).keys()) + list(sender_config.get("channels", {}).keys()):
        if channel_id.startswith('channel'):
            try:
                num = int(channel_id.replace('channel', ''))
                if num > max_num:
                    max_num = num
            except ValueError:
                continue
    
    return {
        "channel_id": f"channel{max_num + 1}",
        "metrics_port": next_metrics_port
    }

# Add this new endpoint
@app.put("/channels/{channel_id}/update")
def update_channel_new(channel_id: str, channel: dict):
    """Update an existing channel with new format"""
    # Check both receiver and sender configs
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    sender_config = load_config(SENDER_CONFIG_FILE)
    
    config = None
    config_file = None
    channel_type = None
    
    if channel_id in receiver_config.get("channels", {}):
        config = receiver_config
        config_file = RECEIVER_CONFIG_FILE
        channel_type = "receive"
    elif channel_id in sender_config.get("channels", {}):
        config = sender_config
        config_file = SENDER_CONFIG_FILE
        channel_type = "send"
    else:
        raise HTTPException(status_code=404, detail="Channel not found")

    logger.info(f"Starting channel update for {channel_id}")
    
    # Convert the input to our expected channel format
    channel_dict = {
        "name": channel["name"],
        "enabled": channel.get("enabled", True),
        "input": channel["input"],
        "output": channel["output"],
        "settings": channel["settings"],
        "metrics_port": channel["metrics_port"],
        "status": "stopped",  # Will be updated later
        "process_id": None,
        "last_error": None
    }
    
    # Update the channel in config
    config["channels"][channel_id] = channel_dict
    save_config(config, config_file)
    
    # Generate service files
    if channel_type == "receive":
        success, error = generate_receiver_service_file(channel_id, channel_dict)
    else:
        success, error = generate_sender_service_file(channel_id, channel_dict)
    
    if not success:
        logger.warning(f"Failed to generate service files for {channel_id}: {error}")
    
    # Reload systemd configuration to recognize the updated service files
    reload_systemd()
    
    # If the channel is running, restart it
    service_prefix = "rist-receiver" if channel_type == "receive" else "rist-sender"
    current_status = subprocess.run(
        ["systemctl", "is-active", f"{service_prefix}-{channel_id}.service"],
        capture_output=True,
        text=True
    ).stdout.strip()
    
    if current_status == "active":
        try:
            subprocess.run(["systemctl", "restart", f"{service_prefix}-{channel_id}.service"], check=True)
            # Let's avoid using asyncio since it might be complex here
            subprocess.run(["systemctl", "restart", f"ffmpeg-{service_prefix.split('-')[1]}-{channel_id}.service"], check=True)
        except subprocess.CalledProcessError as e:
            raise HTTPException(status_code=500, 
                          detail=f"Failed to restart services: {e.stderr.decode() if hasattr(e, 'stderr') else str(e)}")
    
    # Get updated status
    channel_dict["status"] = current_status if current_status == "active" else "stopped"
    channel_dict["channel_type"] = channel_type
    
    return channel_dict

@app.post("/channels/{channel_id}/receive")
def create_receive_channel(channel_id: str, channel: ReceiverChannel):
    """Create a new receive channel"""
    logger.info(f"Creating receive channel: {channel_id} - {channel}")
    config = load_config(RECEIVER_CONFIG_FILE)
    
    # Check if channel already exists in either config
    if channel_id in config.get("channels", {}):
        raise HTTPException(status_code=400, detail="Channel ID already exists in receiver config")
    
    sender_config = load_config(SENDER_CONFIG_FILE)
    if channel_id in sender_config.get("channels", {}):
        raise HTTPException(status_code=400, detail="Channel ID already exists in sender config")
    
    # Assign metrics port if not provided
    if not channel.metrics_port:
        channel.metrics_port = get_next_metrics_port()
    
    # Format the channel data
    channel_dict = {
        "name": channel.name,
        "enabled": channel.enabled,
        "input": f"rist://{channel.source_ip}:{channel.source_port}" + 
                (f"?virt-dst-port={channel.virt_dst_port}" if channel.virt_dst_port else ""),
        "output": f"udp://{channel.multicast_ip}:{channel.multicast_port}",
        "settings": {
            "profile": 1,
            "virt_src_port": channel.virt_dst_port,  # Store as virt_src_port for compatibility
            "buffer": channel.buffer,
            "encryption_type": channel.encryption_type,
            "secret": channel.secret
        },
        "metrics_port": channel.metrics_port,
        "status": "stopped"
    }
    
    # Ensure the channels key exists
    if "channels" not in config:
        config["channels"] = {}
    
    # Add channel to config
    config["channels"][channel_id] = channel_dict
    save_config(config, RECEIVER_CONFIG_FILE)
    
    # Save backup sources if provided
    if channel.backup_sources:
        backup_config = load_config(BACKUP_SOURCES_FILE)
        if "channels" not in backup_config:
            backup_config["channels"] = {}
        
        backup_config["channels"][channel_id] = channel.backup_sources
        save_config(backup_config, BACKUP_SOURCES_FILE)
    
    # Generate service files
    try:
        success, error = generate_receiver_service_file(channel_id, channel_dict)
        if not success:
            logger.warning(f"Failed to generate service files for {channel_id}: {error}")
        else:
            # Enable service if channel is enabled
            if channel.enabled:
                enable_service(f"rist-receiver-{channel_id}.service")
            
            # Reload systemd to recognize the new service
            reload_systemd()
    except Exception as e:
        logger.error(f"Error generating service files: {str(e)}")
        # Continue anyway since the channel is created in the config
    
    logger.info(f"Successfully created receive channel: {channel_id}")
    return {**channel_dict, "channel_type": "receive"}

@app.post("/channels/{channel_id}/send")
def create_send_channel(channel_id: str, channel: SenderChannel):
    """Create a new send channel"""
    logger.info(f"Creating send channel: {channel_id} - {channel}")
    config = load_config(SENDER_CONFIG_FILE)
    
    # Check if channel already exists in either config
    if channel_id in config.get("channels", {}):
        raise HTTPException(status_code=400, detail="Channel ID already exists in sender config")
    
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    if channel_id in receiver_config.get("channels", {}):
        raise HTTPException(status_code=400, detail="Channel ID already exists in receiver config")
    
    # Assign metrics port if not provided
    if not channel.metrics_port:
        channel.metrics_port = get_next_metrics_port()
    
    # Format the channel data
    channel_dict = {
        "name": channel.name,
        "enabled": channel.enabled,
        "input": channel.input_source,
        "output": f"rist://{channel.destination_ip}:{channel.destination_port}" +
                 (f"?virt-dst-port={channel.virt_src_port}" if channel.virt_src_port else ""),
        "settings": {
            "profile": 1,
            "virt_dst_port": channel.virt_src_port,
            "buffer": channel.buffer,
            "encryption_type": channel.encryption_type,
            "secret": channel.secret
        },
        "metrics_port": channel.metrics_port,
        "status": "stopped"
    }
    
    # Ensure the channels key exists
    if "channels" not in config:
        config["channels"] = {}
    
    # Add channel to config
    config["channels"][channel_id] = channel_dict
    save_config(config, SENDER_CONFIG_FILE)
    
    # Generate service files
    try:
        success, error = generate_sender_service_file(channel_id, channel_dict)
        if not success:
            logger.warning(f"Failed to generate service files for {channel_id}: {error}")
        else:
            # Enable service if channel is enabled
            if channel.enabled:
                enable_service(f"rist-sender-{channel_id}.service")
            
            # Reload systemd to recognize the new service
            reload_systemd()
    except Exception as e:
        logger.error(f"Error generating service files: {str(e)}")
        # Continue anyway since the channel is created in the config
    
    logger.info(f"Successfully created send channel: {channel_id}")
    return {**channel_dict, "channel_type": "send"}

@app.get("/channels/{channel_id}")
def get_channel(channel_id: str):
    """Retrieve details of a specific channel"""
    # Check receiver config first
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    if channel_id in receiver_config.get("channels", {}):
        channel = receiver_config["channels"][channel_id]
        
        # Check actual running status
        service_name = f"rist-receiver-{channel_id}.service"
        result = subprocess.run(
            ["systemctl", "is-active", service_name],
            capture_output=True,
            text=True
        )
        channel["status"] = "running" if result.stdout.strip() == "active" else "stopped"
        
        # Save the updated status to config
        receiver_config["channels"][channel_id]["status"] = channel["status"]
        save_config(receiver_config, RECEIVER_CONFIG_FILE)
        
        return {**channel, "channel_type": "receive"}
    
    # Check sender config
    sender_config = load_config(SENDER_CONFIG_FILE)
    if channel_id in sender_config.get("channels", {}):
        channel = sender_config["channels"][channel_id]
        
        # Check actual running status
        service_name = f"rist-sender-{channel_id}.service"
        result = subprocess.run(
            ["systemctl", "is-active", service_name],
            capture_output=True,
            text=True
        )
        channel["status"] = "running" if result.stdout.strip() == "active" else "stopped"
        
        # Save the updated status to config
        sender_config["channels"][channel_id]["status"] = channel["status"]
        save_config(sender_config, SENDER_CONFIG_FILE)
        
        return {**channel, "channel_type": "send"}
    
    # Channel not found in either config
    raise HTTPException(status_code=404, detail="Channel not found")

@app.post("/channels/{channel_id}/generate-services")
def generate_service_files(channel_id: str):
    """Generate systemd service files for a channel"""
    # Determine if this is a receiver or sender channel
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    sender_config = load_config(SENDER_CONFIG_FILE)
    
    if channel_id in receiver_config.get("channels", {}):
        # Generate service files for receiver channel
        channel = receiver_config["channels"][channel_id]
        success, error = generate_receiver_service_file(channel_id, channel)
        
        if not success:
            raise HTTPException(status_code=500, detail=f"Failed to generate receiver service files: {error}")
        
        # Enable service if channel is enabled
        if channel.get("enabled", True):
            enable_service(f"rist-receiver-{channel_id}.service")
            enable_service(f"hls-receiver-{channel_id}.service")  # Add this line
        
        return {"status": "success", "message": "Receiver service files generated successfully"}
    
    elif channel_id in sender_config.get("channels", {}):
        # Generate service files for sender channel
        channel = sender_config["channels"][channel_id]
        success, error = generate_sender_service_file(channel_id, channel)
        
        if not success:
            raise HTTPException(status_code=500, detail=f"Failed to generate sender service files: {error}")
        
        # Enable service if channel is enabled
        if channel.get("enabled", True):
            enable_service(f"rist-sender-{channel_id}.service")
            enable_service(f"hls-sender-{channel_id}.service")  # Add this line

        
        return {"status": "success", "message": "Sender service files generated successfully"}
    
    else:
        raise HTTPException(status_code=404, detail="Channel not found")

@app.put("/channels/{channel_id}/start")
def start_channel(channel_id: str):
    """Start a channel's services"""
    # Check if it's a receiver or sender channel
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    sender_config = load_config(SENDER_CONFIG_FILE)
    
    service_prefix = None
    hls_prefix = None
    if channel_id in receiver_config.get("channels", {}):
        service_prefix = "rist-receiver"
        hls_prefix = "hls-receiver"
    elif channel_id in sender_config.get("channels", {}):
        service_prefix = "rist-sender"
        hls_prefix = "hls-sender"
    else:
        raise HTTPException(status_code=404, detail="Channel not found")
    
    try:
        # First make sure service files exist, if not generate them
        if not os.path.exists(f"{SERVICE_DIR}/{service_prefix}-{channel_id}.service"):
            generate_service_files(channel_id)
        
        # Start the RIST service 
        subprocess.run(["systemctl", "start", f"{service_prefix}-{channel_id}.service"], check=True)
        
        # Explicitly start the HLS service
        subprocess.run(["systemctl", "start", f"{hls_prefix}-{channel_id}.service"], check=True)
        
        # Update channel status in config
        if service_prefix == "rist-receiver":
            receiver_config["channels"][channel_id]["status"] = "running"
            save_config(receiver_config, RECEIVER_CONFIG_FILE)
        else:
            sender_config["channels"][channel_id]["status"] = "running"
            save_config(sender_config, SENDER_CONFIG_FILE)
        
        return {"status": "started"}
    
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Failed to start services: {e.stderr.decode() if hasattr(e, 'stderr') else str(e)}")

@app.put("/channels/{channel_id}/stop")
def stop_channel(channel_id: str):
    """Stop a channel's services"""
    # Check if it's a receiver or sender channel
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    sender_config = load_config(SENDER_CONFIG_FILE)
    
    service_prefix = None
    hls_prefix = None
    if channel_id in receiver_config.get("channels", {}):
        service_prefix = "rist-receiver"
        hls_prefix = "hls-receiver"
    elif channel_id in sender_config.get("channels", {}):
        service_prefix = "rist-sender"
        hls_prefix = "hls-sender"
    else:
        raise HTTPException(status_code=404, detail="Channel not found")
    
    try:
        # Stop the RIST service (HLS will stop automatically due to the BindsTo directive)
        subprocess.run(["systemctl", "stop", f"{service_prefix}-{channel_id}.service"], check=True)
        
        # Update channel status in config
        if service_prefix == "rist-receiver":
            receiver_config["channels"][channel_id]["status"] = "stopped"
            save_config(receiver_config, RECEIVER_CONFIG_FILE)
        else:
            sender_config["channels"][channel_id]["status"] = "stopped"
            save_config(sender_config, SENDER_CONFIG_FILE)
        
        return {"status": "stopped"}
    
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Failed to stop services: {e.stderr.decode() if hasattr(e, 'stderr') else str(e)}")

@app.put("/channels/{channel_id}/ffmpeg/restart")
def restart_ffmpeg(channel_id: str):
    """Restart HLS service for a channel"""
    # Check if it's a receiver or sender channel
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    sender_config = load_config(SENDER_CONFIG_FILE)
    
    service_prefix = None
    if channel_id in receiver_config.get("channels", {}):
        service_prefix = "hls-receiver"
    elif channel_id in sender_config.get("channels", {}):
        service_prefix = "hls-sender"
    else:
        raise HTTPException(status_code=404, detail="Channel not found")
    
    try:
        subprocess.run(["systemctl", "restart", f"{service_prefix}-{channel_id}.service"], check=True)
        return {"status": "restarted"}
    
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Failed to restart HLS service: {e.stderr.decode() if hasattr(e, 'stderr') else str(e)}")


@app.delete("/channels/{channel_id}")
def delete_channel(channel_id: str):
    """Delete a channel and its service files"""
    channel_type = None
    
    # Try to delete from receiver config
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    if channel_id in receiver_config.get("channels", {}):
        # Stop services first
        try:
            subprocess.run(["systemctl", "stop", f"rist-receiver-{channel_id}.service"], check=True)
            # The HLS service should stop automatically due to BindsTo, but we'll make sure
            subprocess.run(["systemctl", "stop", f"hls-receiver-{channel_id}.service"], check=False)
        except:
            pass
        
        # Remove from receiver config
        del receiver_config["channels"][channel_id]
        save_config(receiver_config, RECEIVER_CONFIG_FILE)
        
        # Also remove from backup sources if present
        backup_config = load_config(BACKUP_SOURCES_FILE)
        if channel_id in backup_config.get("channels", {}):
            del backup_config["channels"][channel_id]
            save_config(backup_config, BACKUP_SOURCES_FILE)
        
        channel_type = "receive"
    
    # Try to delete from sender config
    else:
        sender_config = load_config(SENDER_CONFIG_FILE)
        if channel_id in sender_config.get("channels", {}):
            # Stop services first
            try:
                subprocess.run(["systemctl", "stop", f"rist-sender-{channel_id}.service"], check=True)
                # The HLS service should stop automatically due to BindsTo, but we'll make sure
                subprocess.run(["systemctl", "stop", f"hls-sender-{channel_id}.service"], check=False)
            except:
                pass
            
            # Remove from sender config
            del sender_config["channels"][channel_id]
            save_config(sender_config, SENDER_CONFIG_FILE)
            
            channel_type = "send"
        else:
            # Channel not found in either config
            raise HTTPException(status_code=404, detail="Channel not found")
    
    # Remove service files
    try:
        if channel_type == "receive":
            service_prefix = "rist-receiver"
            hls_prefix = "hls-receiver"
        else:  # channel_type == "send"
            service_prefix = "rist-sender"
            hls_prefix = "hls-sender"
        
        # Disable services
        try:
            subprocess.run(["systemctl", "disable", f"{service_prefix}-{channel_id}.service"], check=True)
            subprocess.run(["systemctl", "disable", f"{hls_prefix}-{channel_id}.service"], check=True)
        except:
            pass
        
        # Remove service files
        service_file = f"{SERVICE_DIR}/{service_prefix}-{channel_id}.service"
        hls_file = f"{SERVICE_DIR}/{hls_prefix}-{channel_id}.service"
        
        if os.path.exists(service_file):
            os.remove(service_file)
        
        if os.path.exists(hls_file):
            os.remove(hls_file)
        
        # Reload systemd to recognize the changes
        reload_systemd()
    
    except Exception as e:
        logger.error(f"Error removing service files: {str(e)}")
        # Continue anyway since the channel is already removed from config
    
    return {"status": "deleted", "channel_type": channel_type}

@app.get("/channels/{channel_id}/backup-sources")
def get_backup_sources(channel_id: str):
    """Get backup sources for a receive channel"""
    # Check if it's a receive channel
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    if channel_id not in receiver_config.get("channels", {}):
        raise HTTPException(status_code=404, detail="Receive channel not found")
    
    # Get backup sources
    backup_config = load_config(BACKUP_SOURCES_FILE)
    backup_sources = backup_config.get("channels", {}).get(channel_id, [])
    
    return {
        "channel_id": channel_id,
        "backup_sources": backup_sources
    }

@app.put("/channels/{channel_id}/backup-sources")
def update_backup_sources(channel_id: str, backup_sources: List[str] = Body(...)):
    """Update backup sources for a receive channel"""
    # Check if it's a receive channel
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    if channel_id not in receiver_config.get("channels", {}):
        raise HTTPException(status_code=404, detail="Receive channel not found")
    
    # Update backup sources
    backup_config = load_config(BACKUP_SOURCES_FILE)
    if "channels" not in backup_config:
        backup_config["channels"] = {}
    
    # Clean up sources (remove empty strings and duplicates)
    clean_sources = []
    seen = set()
    for source in backup_sources:
        source = source.strip()
        if source and source not in seen:
            clean_sources.append(source)
            seen.add(source)
    
    if clean_sources:
        backup_config["channels"][channel_id] = clean_sources
    elif channel_id in backup_config["channels"]:
        del backup_config["channels"][channel_id]
    
    save_config(backup_config, BACKUP_SOURCES_FILE)
    
    return {
        "channel_id": channel_id,
        "backup_sources": clean_sources
    }

@app.get("/channels/{channel_id}/backup-health")
def get_channel_backup_health(channel_id: str):
    """
    Check backup health status for a channel
    """
    try:
        # Look for a health status file created by the failover script
        health_status_file = f"/root/ristgateway/{channel_id}_backup_health.json"
        
        if os.path.exists(health_status_file):
            with open(health_status_file, 'r') as f:
                import json
                health_data = json.load(f)
            
            return {
                "channel_id": channel_id,
                "has_backups": health_data.get('has_backups', False),
                "is_healthy": health_data.get('is_healthy', False),
                "last_checked": health_data.get('last_checked', None)
            }
        else:
            # If no health status file exists, assume no backups
            return {
                "channel_id": channel_id,
                "has_backups": False,
                "is_healthy": False,
                "last_checked": None
            }
    
    except Exception as e:
        logger.error(f"Failed to retrieve backup health for {channel_id}: {e}")
        return {
            "channel_id": channel_id,
            "has_backups": False,
            "is_healthy": False,
            "last_checked": None
        }

@app.post("/channels/{channel_id}/keepalive")
async def channel_keepalive(channel_id: str):
    """Handle keepalive request for channel"""
    # Check if the channel exists in either config
    receiver_config = load_config(RECEIVER_CONFIG_FILE)
    sender_config = load_config(SENDER_CONFIG_FILE)
    
    channel_exists = (channel_id in receiver_config.get("channels", {}) or 
                      channel_id in sender_config.get("channels", {}))
    
    if not channel_exists:
        raise HTTPException(status_code=404, detail="Channel not found")
    
    # Determine the service prefix based on channel type
    service_prefix = None
    if channel_id in receiver_config.get("channels", {}):
        service_prefix = "ffmpeg-receiver"
    else:
        service_prefix = "ffmpeg-sender"
    
    # Start FFmpeg if it's not running
    try:
        result = subprocess.run(
            ["systemctl", "is-active", f"{service_prefix}-{channel_id}.service"],
            capture_output=True,
            text=True
        )
        
        if result.stdout.strip() != "active":
            # Start the FFmpeg service
            subprocess.run(["systemctl", "start", f"{service_prefix}-{channel_id}.service"], check=True)
    
    except Exception as e:
        logger.error(f"Error in keepalive for {channel_id}: {str(e)}")
        # Continue anyway, as this is just a keepalive request
    
    return {
        "status": "ok", 
        "channel_id": channel_id,
        "timestamp": datetime.datetime.now().isoformat()
    }

@app.get("/channels/{channel_id}/metrics")
def get_channel_metrics(channel_id: str):
    """
    Retrieve metrics for a specific channel with caching and error handling
    """
    try:
        config = load_config(RECEIVER_CONFIG_FILE)
        if channel_id not in config["channels"]:
            logger.error(f"Channel {channel_id} not found")
            raise HTTPException(status_code=404, detail="Channel not found")
        
        channel = config["channels"][channel_id]
        metrics_port = channel.get('metrics_port')
        
        if not metrics_port:
            logger.error(f"No metrics port configured for channel {channel_id}")
            raise HTTPException(status_code=500, detail="Metrics port not configured")
        
        # Retrieve cached or fresh metrics
        metrics = get_cached_metrics(channel_id, metrics_port)
        
        return metrics
    
    except Exception as e:
        logger.error(f"Unexpected error in metrics retrieval: {e}")
        logger.error(traceback.format_exc())
        raise HTTPException(status_code=500, detail="Internal server error retrieving metrics")



@app.get("/channels/{channel_id}/metrics2")
def get_channel_metrics(channel_id: str):
    config = load_config(SENDER_CONFIG_FILE)
    if channel_id not in config["channels"]:
        raise HTTPException(status_code=404, detail="Channel not found")
    
    try:
        channel = config["channels"][channel_id]
        metrics = get_cached_metrics2(channel_id, channel["metrics_port"])
        
        # Structure the response to match the expected format
        response = {
            "input": {
                "peers": metrics.get("peers", []),
                "quality": metrics.get("quality", 100.0),
                "total_bandwidth_bps": metrics.get("total_bandwidth_bps", 0),
                "total_retry_bandwidth_bps": metrics.get("total_retry_bandwidth_bps", 0),
                "timing": metrics.get("timing", {
                    "min_iat": 0,
                    "cur_iat": 0,
                    "max_iat": 0,
                    "rtt": 0
                }),
                "packets": metrics.get("packets", {
                    "sent": 0,
                    "received": 0,
                    "missing": 0,
                    "reordered": 0,
                    "recovered": 0,
                    "recovered_one_retry": 0,
                    "lost": 0
                })
            },
            "output": metrics
        }
        
        return response
        
    except Exception as e:
        logger.error(f"Error fetching metrics: {e}")
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/channels/{channel_id}/network-flow")
def get_channel_network_flow(channel_id: str):
    """
    Generate a network flow visualization for a specific channel,
    showing both upstream and downstream connections including transcoders
    """
    try:
        # First get the channel from existing endpoint to ensure it exists and check status
        try:
            # This will use your existing get_channel endpoint
            channel_data = get_channel(channel_id)
            if channel_data["status"] != "running":
                raise HTTPException(status_code=400, detail="Channel is not running")
        except HTTPException as e:
            if e.status_code == 404:
                raise HTTPException(status_code=404, detail="Channel not found")
            raise e

        # Determine channel type from the returned data
        channel_type = channel_data.get("channel_type")
        logger.info(f"Processing network flow for channel {channel_id}, type: {channel_type}")
        
        # Load all channels for looking up connections
        all_channels = get_channels()  # This gets all channels with their status
        logger.info(f"Found {len(all_channels)} total channels (receivers + senders)")
        
        # Also get all transcoders
        try:
            import requests
            transcoders_response = requests.get("http://localhost:5000/transcoders", timeout=2)
            if transcoders_response.status_code == 200:
                all_transcoders = transcoders_response.json()
                logger.info(f"Found {len(all_transcoders)} transcoders")
            else:
                logger.warning(f"Failed to get transcoders: {transcoders_response.status_code}")
                all_transcoders = {}
        except Exception as e:
            logger.error(f"Error getting transcoders: {e}")
            all_transcoders = {}
        
        # Build network flow based on channel type
        result = {
            "id": channel_id,
            "name": channel_data.get("name", f"Channel {channel_id}"),
            "type": channel_type,
            "upstream": None,
            "downstream": []
        }
        
        # Get metrics for the channel
        try:
            if channel_type == "receive":
                metrics = get_cached_metrics(channel_id, channel_data["metrics_port"])
                result["metrics"] = metrics
            else:  # send
                metrics = get_cached_metrics2(channel_id, channel_data["metrics_port"])
                result["metrics"] = metrics
        except Exception as e:
            logger.error(f"Error getting metrics for {channel_type} {channel_id}: {str(e)}")
            result["metrics"] = {}
        
        # Build the upstream connection (what feeds into this channel)
        if channel_type == "receive":
            # Receivers get input from external RIST sources
            result["upstream"] = {
                "type": "external_rist",
                "url": channel_data["input"],
                "metrics": {
                    "quality": result.get("metrics", {}).get("quality", 100.0),
                    "bandwidth_bps": result.get("metrics", {}).get("bandwidth_bps", 0)
                }
            }
        elif channel_type == "send":
            # Figure out what's feeding this sender
            input_url = channel_data["input"]
            logger.info(f"Sender channel {channel_id} has input URL: {input_url}")
            
            # Find if any receiver outputs to this sender's input
            upstream_found = False
            
            # Check all channels (receivers) for matching output
            for up_id, up_channel in all_channels.items():
                up_output = up_channel.get("output", "")
                up_status = up_channel.get("status", "")
                
                if up_channel.get("status") == "running" and up_channel.get("output") == input_url:
                    # This channel feeds into our sender
                    up_type = up_channel.get("channel_type", "")
                    logger.info(f"Found upstream channel {up_id} with type {up_type}")
                    
                    upstream_found = True
                    result["upstream"] = {
                        "type": "receiver",
                        "id": up_id,
                        "name": up_channel.get("name", f"Receiver {up_id}"),
                        "url": input_url,
                        "metrics": {}
                    }
                    
                    try:
                        # Get metrics based on the upstream type
                        upstream_metrics = get_cached_metrics(up_id, up_channel["metrics_port"])
                        result["upstream"]["metrics"] = {
                            "quality": upstream_metrics.get("quality", 100.0),
                            "bandwidth_bps": upstream_metrics.get("bandwidth_bps", 0)
                        }
                    except Exception as e:
                        logger.error(f"Error getting upstream metrics: {e}")
                    break
            
            # If no receiver upstream found, check transcoders
            if not upstream_found:
                for up_id, up_channel in all_transcoders.items():
                    transcoder_output_address = up_channel.get("output", {}).get("address", "")
                    logger.info(f"Checking transcoder {up_id} output: {transcoder_output_address} against {input_url}")
                    
                    if up_channel.get("status") == "running" and transcoder_output_address == input_url:
                        logger.info(f"Found upstream transcoder {up_id}")
                        
                        upstream_found = True
                        result["upstream"] = {
                            "type": "transcoder",
                            "id": up_id,
                            "name": up_channel.get("name", f"Transcoder {up_id}"),
                            "url": input_url,
                            "metrics": {}
                        }
                        
                        try:
                            # Get transcoder metrics
                            transcoder_metrics_response = requests.get(f"http://localhost:5000/transcoders/{up_id}/metrics", timeout=2)
                            if transcoder_metrics_response.status_code == 200:
                                transcoder_metrics = transcoder_metrics_response.json()
                                result["upstream"]["metrics"] = {
                                    "quality": 100.0,  # Assume good quality for transcoders
                                    "bandwidth_bps": transcoder_metrics.get("output_bitrate", 0) * 1000  # Convert to bps
                                }
                        except Exception as e:
                            logger.error(f"Error getting transcoder metrics: {e}")
                        break
            
            # If no upstream found, it's an external source
            if not upstream_found:
                logger.info(f"No upstream channel found for sender {channel_id}, treating as external UDP source")
                result["upstream"] = {
                    "type": "external_udp",
                    "url": input_url,
                    "metrics": {
                        "quality": 100.0,  # Assume good quality for external sources
                        "bandwidth_bps": result.get("metrics", {}).get("total_bandwidth_bps", 0)
                    }
                }
        
        # Build downstream connections - this should detect all types (senders, transcoders, etc.)
        output_url = channel_data["output"]
        logger.info(f"Channel {channel_id} has output URL: {output_url}")
        
        # Check for downstream senders
        for down_id, down_channel in all_channels.items():
            # Skip the current channel
            if down_id == channel_id:
                continue
                
            # Check if this channel uses our output as input and is running
            if down_channel.get("status") == "running" and down_channel.get("input") == output_url:
                # This is a downstream sender
                down_type = down_channel.get("channel_type", "")
                logger.info(f"Found downstream channel {down_id} with type {down_type}")
                
                downstream_node = {
                    "type": "sender",
                    "id": down_id,
                    "name": down_channel.get("name", f"Sender {down_id}"),
                    "url": down_channel.get("output", ""),
                    "metrics": {},
                    "clients": []
                }
                
                # Get metrics for this sender
                try:
                    downstream_metrics = get_cached_metrics2(down_id, down_channel["metrics_port"])
                    downstream_node["metrics"] = {
                        "quality": downstream_metrics.get("quality", 100.0),
                        "bandwidth_bps": downstream_metrics.get("total_bandwidth_bps", 0)
                    }
                    
                    # For senders, add connected clients
                    for peer in downstream_metrics.get("peers", []):
                        client = {
                            "id": peer.get("id", "unknown"),
                            "name": peer.get("cname", "Client"),
                            "url": peer.get("peer_url", ""),
                            "metrics": {
                                "quality": peer.get("packets", {}).get("quality", 100.0),
                                "bandwidth_bps": peer.get("bandwidth_bps", 0)
                            }
                        }
                        downstream_node["clients"].append(client)
                except Exception as e:
                    logger.error(f"Error getting downstream metrics for {down_id}: {e}")
                
                result["downstream"].append(downstream_node)
        
        # Check for downstream transcoders
        for down_id, down_channel in all_transcoders.items():
            transcoder_input_address = down_channel.get("input", {}).get("address", "")
            logger.info(f"Checking transcoder {down_id} input: {transcoder_input_address} against {output_url}")
            
            if down_channel.get("status") == "running" and transcoder_input_address == output_url:
                # This is a downstream transcoder
                logger.info(f"Found downstream transcoder {down_id}")
                
                transcoder_node = {
                    "type": "transcoder",
                    "id": down_id,
                    "name": down_channel.get("name", f"Transcoder {down_id}"),
                    "url": down_channel.get("output", {}).get("address", ""),
                    "metrics": {},
                    "downstream": []
                }
                
                # Get metrics for this transcoder
                try:
                    transcoder_metrics_response = requests.get(f"http://localhost:5000/transcoders/{down_id}/metrics", timeout=2)
                    if transcoder_metrics_response.status_code == 200:
                        transcoder_metrics = transcoder_metrics_response.json()
                        transcoder_node["metrics"] = {
                            "quality": 100.0,  # Assume good quality for transcoders
                            "bandwidth_bps": transcoder_metrics.get("output_bitrate", 0) * 1000  # Convert to bps
                        }
                except Exception as e:
                    logger.error(f"Error getting transcoder metrics: {e}")
                
                # Check for channels that use this transcoder's output
                transcoder_output = down_channel.get("output", {}).get("address", "")
                logger.info(f"Transcoder {down_id} output: {transcoder_output}")
                
                # Look for senders that use this transcoder's output
                for next_id, next_channel in all_channels.items():
                    next_input = next_channel.get("input", "")
                    logger.info(f"Checking if channel {next_id} uses transcoder output: {next_input} == {transcoder_output}")
                    
                    if next_channel.get("status") == "running" and next_input == transcoder_output:
                        # This channel takes input from our transcoder
                        logger.info(f"Found channel {next_id} using transcoder {down_id} output")
                        
                        next_node = {
                            "type": "sender",
                            "id": next_id,
                            "name": next_channel.get("name", f"Sender {next_id}"),
                            "url": next_channel.get("output", ""),
                            "metrics": {},
                            "clients": []
                        }
                        
                        # Get metrics for this sender
                        try:
                            next_metrics = get_cached_metrics2(next_id, next_channel["metrics_port"])
                            next_node["metrics"] = {
                                "quality": next_metrics.get("quality", 100.0),
                                "bandwidth_bps": next_metrics.get("total_bandwidth_bps", 0)
                            }
                            
                            # Add clients connected to this sender
                            for peer in next_metrics.get("peers", []):
                                client = {
                                    "id": peer.get("id", "unknown"),
                                    "name": peer.get("cname", "Client"),
                                    "url": peer.get("peer_url", ""),
                                    "metrics": {
                                        "quality": peer.get("packets", {}).get("quality", 100.0),
                                        "bandwidth_bps": peer.get("bandwidth_bps", 0)
                                    }
                                }
                                next_node["clients"].append(client)
                        except Exception as e:
                            logger.error(f"Error getting metrics for channel using transcoder output: {e}")
                        
                        transcoder_node["downstream"].append(next_node)
                
                result["downstream"].append(transcoder_node)
        
        # Special case: if this IS a transcoder, handle it differently
        if channel_id.startswith("transcoder"):
            try:
                # Get transcoder details
                transcoder_response = requests.get(f"http://localhost:5000/transcoders/{channel_id}", timeout=2)
                if transcoder_response.status_code == 200:
                    transcoder_data = transcoder_response.json()
                    
                    # Update the result type
                    result["type"] = "transcoder"
                    
                    # Set upstream based on input address
                    input_address = transcoder_data.get("input", {}).get("address", "")
                    logger.info(f"Transcoder {channel_id} has input address: {input_address}")
                    
                    # Find what's feeding this transcoder
                    upstream_found = False
                    for up_id, up_channel in all_channels.items():
                        if up_channel.get("status") == "running" and up_channel.get("output") == input_address:
                            upstream_found = True
                            result["upstream"] = {
                                "type": "receiver",
                                "id": up_id,
                                "name": up_channel.get("name", f"Receiver {up_id}"),
                                "url": input_address,
                                "metrics": {}
                            }
                            
                            try:
                                upstream_metrics = get_cached_metrics(up_id, up_channel["metrics_port"])
                                result["upstream"]["metrics"] = {
                                    "quality": upstream_metrics.get("quality", 100.0),
                                    "bandwidth_bps": upstream_metrics.get("bandwidth_bps", 0)
                                }
                            except Exception as e:
                                logger.error(f"Error getting upstream metrics for transcoder: {e}")
                            break
                    
                    # If no upstream found, it's an external source
                    if not upstream_found:
                        result["upstream"] = {
                            "type": "external_udp",
                            "url": input_address,
                            "metrics": {
                                "quality": 100.0,
                                "bandwidth_bps": 0
                            }
                        }
                    
                    # Get transcoder metrics
                    try:
                        metrics_response = requests.get(f"http://localhost:5000/transcoders/{channel_id}/metrics", timeout=2)
                        if metrics_response.status_code == 200:
                            result["metrics"] = metrics_response.json()
                    except Exception as e:
                        logger.error(f"Error getting transcoder metrics: {e}")
                    
                    # Find downstream channels (senders using this transcoder's output)
                    output_address = transcoder_data.get("output", {}).get("address", "")
                    logger.info(f"Transcoder {channel_id} has output address: {output_address}")
                    
                    for down_id, down_channel in all_channels.items():
                        if down_channel.get("status") == "running" and down_channel.get("input") == output_address:
                            logger.info(f"Found sender {down_id} using transcoder {channel_id} output")
                            
                            sender_node = {
                                "type": "sender",
                                "id": down_id,
                                "name": down_channel.get("name", f"Sender {down_id}"),
                                "url": down_channel.get("output", ""),
                                "metrics": {},
                                "clients": []
                            }
                            
                            try:
                                sender_metrics = get_cached_metrics2(down_id, down_channel["metrics_port"])
                                sender_node["metrics"] = {
                                    "quality": sender_metrics.get("quality", 100.0),
                                    "bandwidth_bps": sender_metrics.get("total_bandwidth_bps", 0)
                                }
                                
                                # Add clients
                                for peer in sender_metrics.get("peers", []):
                                    client = {
                                        "id": peer.get("id", "unknown"),
                                        "name": peer.get("cname", "Client"),
                                        "url": peer.get("peer_url", ""),
                                        "metrics": {
                                            "quality": peer.get("packets", {}).get("quality", 100.0),
                                            "bandwidth_bps": peer.get("bandwidth_bps", 0)
                                        }
                                    }
                                    sender_node["clients"].append(client)
                            except Exception as e:
                                logger.error(f"Error getting sender metrics for transcoder output: {e}")
                            
                            result["downstream"].append(sender_node)
            except Exception as e:
                logger.error(f"Error handling transcoder channel: {e}")
        
        # Special case for senders - add direct clients
        if channel_type == "send":
            for peer in result.get("metrics", {}).get("peers", []):
                client = {
                    "type": "client",
                    "id": peer.get("id", "unknown"),
                    "name": peer.get("cname", "Client"),
                    "url": peer.get("peer_url", ""),
                    "metrics": {
                        "quality": peer.get("packets", {}).get("quality", 100.0),
                        "bandwidth_bps": peer.get("bandwidth_bps", 0)
                    }
                }
                result["downstream"].append(client)
        
        logger.info(f"Completed network flow for {channel_id}, found {len(result['downstream'])} downstream connections")
        return result
    
    except HTTPException:
        raise  # Re-raise HTTP exceptions
    except Exception as e:
        logger.error(f"Error generating network flow for {channel_id}: {str(e)}")
        logger.error(traceback.format_exc())
        raise HTTPException(status_code=500, detail=f"Error generating network flow: {str(e)}")


@app.get("/channels/{channel_id}/media-info")
def get_channel_media_info(channel_id: str):
    """Retrieve media information for a specific channel"""
    config = load_config(RECEIVER_CONFIG_FILE)
    if channel_id not in config["channels"]:
        raise HTTPException(status_code=404)
    
    channel = config["channels"][channel_id]
    try:
        cmd = [
            "ffprobe",
            "-v", "quiet",
            "-print_format", "json",
            "-show_format",
            "-show_streams",
            "-show_programs",
            "-i", channel["output"]
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return json.loads(result.stdout)
    except subprocess.CalledProcessError as e:
        logger.error(f"FFprobe error for channel {channel_id}: {e.stderr}")
        raise HTTPException(status_code=500, detail=f"FFprobe error: {e.stderr}")
    except json.JSONDecodeError:
        logger.error(f"Failed to parse FFprobe output for channel {channel_id}")
        raise HTTPException(status_code=500, detail="Failed to parse FFprobe output")



@app.get("/health/metrics")
def health_metrics():
    """
    Return system health metrics
    """
    try:
        import psutil
        import datetime
        global network_stats_history
        
        current_time = time.time()
        
        # CPU Information
        cpu_percent = psutil.cpu_percent(interval=1)
        cpu_cores = [{'core': i, 'usage': percent} for i, percent in enumerate(psutil.cpu_percent(interval=0.1, percpu=True))]

        # Memory Information
        memory = psutil.virtual_memory()

        # Disk Information
        disk = psutil.disk_usage('/')

        # Network Information
        current_network_stats = {}
        for interface, stats in psutil.net_io_counters(pernic=True).items():
            # Skip loopback interface
            if interface.startswith('lo'):
                continue
            
            current_network_stats[interface] = {
                'bytes_sent': stats.bytes_sent,
                'bytes_recv': stats.bytes_recv
            }
        
        # Calculate bandwidth using historical data
        network_stats = calculate_network_bandwidth(current_network_stats, current_time)

        # Construct metrics dictionary
        metrics = {
            'cpu': {
                'average': cpu_percent,
                'cores': cpu_cores
            },
            'memory': {
                'total': memory.total,
                'used': memory.used,
                'used_percent': memory.percent
            },
            'disk': {
                'total': disk.total,
                'used': disk.used,
                'used_percent': disk.percent
            },
            'temperature': 0,  # Default to 0 if temperature sensor not available
            'network': network_stats,
            'timestamp': datetime.datetime.now().isoformat()
        }
        
        # Try to get CPU temperature if available
        try:
            if hasattr(psutil, 'sensors_temperatures'):
                temps = psutil.sensors_temperatures()
                if 'coretemp' in temps and temps['coretemp']:
                    metrics['temperature'] = temps['coretemp'][0].current
        except:
            pass
        
        return metrics
    
    except Exception as e:
        logger.error(f"Error fetching system health metrics: {str(e)}")
        return {
            'error': 'Failed to retrieve system metrics',
            'timestamp': datetime.datetime.now().isoformat()
        }

def calculate_network_bandwidth(current_stats, current_timestamp):
    """Calculate network bandwidth per second"""
    global network_stats_history
    
    if not network_stats_history:
        # Initialize history for first run
        for interface, stats in current_stats.items():
            network_stats_history[interface] = {
                'timestamps': [current_timestamp],
                'bytes_sent': [stats['bytes_sent']],
                'bytes_recv': [stats['bytes_recv']]
            }
        return {
            interface: {
                'bytes_sent': stats['bytes_sent'],
                'bytes_recv': stats['bytes_recv'],
                'bytes_sent_per_sec': 0,
                'bytes_recv_per_sec': 0
            } for interface, stats in current_stats.items()
        }
    
    bandwidth_stats = {}
    
    for interface, current_interface_stats in current_stats.items():
        if interface not in network_stats_history:
            # Handle new interfaces that weren't seen before
            network_stats_history[interface] = {
                'timestamps': [current_timestamp],
                'bytes_sent': [current_interface_stats['bytes_sent']],
                'bytes_recv': [current_interface_stats['bytes_recv']]
            }
            bandwidth_stats[interface] = {
                'bytes_sent': current_interface_stats['bytes_sent'],
                'bytes_recv': current_interface_stats['bytes_recv'],
                'bytes_sent_per_sec': 0,
                'bytes_recv_per_sec': 0
            }
            continue
        
        # Update history with new data points
        history = network_stats_history[interface]
        history['timestamps'].append(current_timestamp)
        history['bytes_sent'].append(current_interface_stats['bytes_sent'])
        history['bytes_recv'].append(current_interface_stats['bytes_recv'])
        
        # Keep only the last 5 data points for a moving average
        history['timestamps'] = history['timestamps'][-5:]
        history['bytes_sent'] = history['bytes_sent'][-5:]
        history['bytes_recv'] = history['bytes_recv'][-5:]
        
        # Calculate bandwidth based on the difference between oldest and newest data points
        if len(history['timestamps']) > 1:
            time_diff = history['timestamps'][-1] - history['timestamps'][0]
            bytes_sent_diff = history['bytes_sent'][-1] - history['bytes_sent'][0]
            bytes_recv_diff = history['bytes_recv'][-1] - history['bytes_recv'][0]
            
            bytes_sent_per_sec = max(0, int(bytes_sent_diff / max(time_diff, 1)))
            bytes_recv_per_sec = max(0, int(bytes_recv_diff / max(time_diff, 1)))
        else:
            bytes_sent_per_sec = 0
            bytes_recv_per_sec = 0
        
        bandwidth_stats[interface] = {
            'bytes_sent': current_interface_stats['bytes_sent'],
            'bytes_recv': current_interface_stats['bytes_recv'],
            'bytes_sent_per_sec': bytes_sent_per_sec,
            'bytes_recv_per_sec': bytes_recv_per_sec
        }
    
    return bandwidth_stats



# Run the app directly if this file is executed
if __name__ == "__main__":
    import uvicorn
    import datetime
    uvicorn.run(app, host="0.0.0.0", port=5000)