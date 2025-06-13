# RIST Gateway

A comprehensive RIST (Reliable Internet Stream Transport) gateway solution with built-in transcoding capabilities powered by GStreamer.

## Features

- **RIST Protocol Support**: Send and receive streams using the RIST protocol
- **Real-time Transcoding**: Hardware-accelerated video transcoding using GStreamer
- **Multi-channel Support**: Manage multiple RIST channels simultaneously
- **Web Interface**: User-friendly web dashboard for configuration and monitoring
- **Automatic Failover**: Built-in failover mechanism for backup sources
- **HLS Streaming**: Convert RIST streams to HLS for web delivery
- **GPU Acceleration**: Support for NVIDIA NVENC, Intel QuickSync, and AMD hardware encoding
- **RESTful API**: Complete API for programmatic control
- **Real-time Metrics**: Monitor bitrate, packet loss, and stream health

## System Requirements

- Ubuntu 20.04 LTS or newer (tested on Ubuntu 22.04)
- Minimum 4GB RAM (8GB recommended)
- Root access for installation
- Network connectivity
- Optional: Compatible GPU for hardware acceleration

## Quick Installation

Run the following command as root:

```bash
curl -sSL "https://raw.githubusercontent.com/caritechsolutions/ristgateway/main/install.sh?$(date +%s)" | sudo bash
```

This will:
- Install all required dependencies
- Set up GStreamer with necessary plugins
- Configure the web interface
- Set up systemd services
- Install WireGuard (requires manual configuration)
- Create a 2GB tmpfs mount for temporary stream storage

## Manual Installation

If you prefer to install manually:

```bash
# Clone the repository
git clone https://github.com/caritechsolutions/ristgateway.git
cd ristgateway

# Make the install script executable
chmod +x install.sh

# Run the installation
sudo ./install.sh
```

## Post-Installation Setup

### 1. Configure WireGuard (Optional)

If you need VPN connectivity:

```bash
# Edit the WireGuard configuration
sudo nano /etc/wireguard/wg0.conf

# Start WireGuard
sudo systemctl enable wg-quick@wg0
sudo systemctl start wg-quick@wg0
```

### 2. Access the Web Interface

Open your browser and navigate to:
- Web Interface: `http://YOUR_SERVER_IP/`
- API Endpoint: `http://YOUR_SERVER_IP:5000/`

### 3. Verify Services

Check that all services are running:

```bash
# Check main gateway service
sudo systemctl status rist-gateway

# Check failover service (if enabled)
sudo systemctl status rist-failover

# Check nginx
sudo systemctl status nginx
```

## Configuration

### Receiver Configuration

Receivers are configured through the web interface or by editing `/root/ristgateway/receiver_config.yaml`:

```yaml
channels:
  channel1:
    name: "Main Stream"
    enabled: true
    source_ip: "239.1.1.1"
    source_port: 5000
    multicast_ip: "224.2.2.2"
    multicast_port: 10000
    buffer: 1000
    metrics_port: 8001
```

### Sender Configuration

Senders are configured through `/root/ristgateway/sender_config.yaml`:

```yaml
channels:
  sender1:
    name: "Output Stream"
    enabled: true
    input_source: "udp://127.0.0.1:10000"
    destination_ip: "192.168.1.100"
    destination_port: 5000
    buffer: 1000
```

### Transcoder Configuration

Transcoders can be created through the web interface with options for:
- Video codec (H.264, H.265, MPEG-2)
- Audio codec (AAC, MP2, AC3)
- Bitrate settings
- Resolution and framerate
- Hardware acceleration device selection

## API Usage

### Get All Channels
```bash
curl http://localhost:5000/channels
```

### Create a Receiver Channel
```bash
curl -X POST http://localhost:5000/receiver/channels \
  -H "Content-Type: application/json" \
  -d '{
    "name": "Test Channel",
    "source_ip": "239.1.1.1",
    "source_port": 5000,
    "multicast_ip": "224.2.2.2",
    "multicast_port": 10000
  }'
```

### Start a Channel
```bash
curl -X POST http://localhost:5000/receiver/channels/channel1/start
```

### Get Metrics
```bash
curl http://localhost:5000/receiver/channels/channel1/metrics
```

## Transcoding

The gateway includes a powerful transcoding engine based on GStreamer. 

### Supported Input Codecs
- H.264/AVC
- H.265/HEVC
- MPEG-2
- AAC, MP2, AC3 (audio)

### Supported Output Codecs
- H.264 (x264, NVENC, VAAPI)
- H.265 (x265, NVENC, VAAPI)
- MPEG-2
- AAC, MP2, AC3 (audio)

### Hardware Acceleration
- **NVIDIA**: Requires NVIDIA driver and CUDA toolkit
- **Intel**: Requires Intel Media Driver
- **AMD**: Requires AMD GPU with VAAPI support

## Directory Structure

```
/root/ristgateway/
├── gateway_api.py          # Main API service
├── transcoder_api.py       # Transcoder management
├── rist-failover.py        # Failover service
├── basic_transcoder        # Transcoder binary
├── hls_server             # HLS server binary
├── web/                   # Web interface files
├── receiver_config.yaml   # Receiver configuration
├── sender_config.yaml     # Sender configuration
└── transcoder_config.yaml # Transcoder configuration

/var/www/html/             # Web interface deployment
/var/www/html/content/     # Temporary stream storage (tmpfs)
/var/log/ristgateway/      # Log files
```

## Troubleshooting

### Service Won't Start
```bash
# Check logs
sudo journalctl -u rist-gateway -n 50
sudo tail -f /var/log/ristgateway/gateway.log
```

### No Video in Web Player
- Ensure the multicast stream is accessible
- Check firewall rules for multicast traffic
- Verify the source is sending data

### High CPU Usage
- Consider enabling hardware acceleration
- Reduce transcoding resolution or bitrate
- Check for multiple channels running simultaneously

### GPU Not Detected
```bash
# Check NVIDIA GPUs
nvidia-smi

# Check Intel GPU
vainfo

# Check available GStreamer plugins
gst-inspect-1.0 | grep -i nvenc
gst-inspect-1.0 | grep -i vaapi
```

## Logs

Log files are located in `/var/log/ristgateway/`:
- `gateway.log` - Main API service logs
- `gateway-error.log` - API error logs
- `failover.log` - Failover service logs
- Individual channel logs in systemd journal

View logs:
```bash
# API logs
sudo tail -f /var/
