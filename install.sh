#!/bin/bash

# Exit on error
set -e

# Clean up any previous installation attempts
rm -rf /root/ristgateway

echo "Starting RIST Gateway installation..."

# Function to check if script is run as root
check_root() {
    if [ "$(id -u)" != "0" ]; then
        echo "This script must be run as root" 1>&2
        exit 1
    fi
}

# Function to download with cache bypass
download_file() {
    local url="$1"
    local output="$2"
    local timestamp=$(date +%s)
    curl -sSL "${url}?v=${timestamp}" -o "$output"
}

# Update system and install dependencies
install_dependencies() {
    echo "Updating system and installing dependencies..."
    apt-get update
    apt-get upgrade -y
    
    # Install git if not present
    apt-get install -y git
    
    # Install system packages including GStreamer
    apt-get install -y python3 python3-pip python3-dev python3-psutil python3-yaml \
    build-essential cmake pkg-config nginx redis-server ffmpeg \
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
    gstreamer1.0-x gstreamer1.0-vaapi gstreamer1.0-nice gstreamer1.0-rtsp \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libsoup2.4-dev libjson-glib-dev libglib2.0-dev \
    vainfo intel-media-va-driver i965-va-driver pciutils
}

install_python_deps() {
    echo "Installing Python dependencies..."
    apt-get update
    
    # First ensure pip is installed
    apt-get install -y python3-pip
    
    # Try installing with apt-get first
    if ! apt-get install -y \
        python3-fastapi \
        python3-flask \
        python3-flask-cors \
        python3-psutil \
        python3-pydantic \
        python3-yaml \
        python3-uvicorn \
        python3-aiofiles \
        python3-requests; then
        
        echo "Some packages weren't available via apt. Installing missing packages via pip..."
        
        # Install packages that might have failed via pip
        pip3 install fastapi
        pip3 install "uvicorn[standard]"
    fi
    
    # Check pip version to determine if --break-system-packages is supported
    PIP_VERSION=$(pip3 --version | awk '{print $2}')
    PIP_MAJOR=$(echo $PIP_VERSION | cut -d. -f1)
    PIP_MINOR=$(echo $PIP_VERSION | cut -d. -f2)
    
    if [ "$PIP_MAJOR" -gt 23 ] || ([ "$PIP_MAJOR" -eq 23 ] && [ "$PIP_MINOR" -ge 0 ]); then
        pip3 install GPUtil --break-system-packages
    else
        pip3 install GPUtil
    fi
}

# Clone repository
clone_repo() {
    echo "Cloning RIST gateway repository..."
    mkdir -p /root/ristgateway
    cd /root
    rm -rf ristgateway/*
    # Replace with your actual repository URL
    git clone https://github.com/yourusername/rist_gateway.git ristgateway
    chmod -R 755 /root/ristgateway
}

# Stop all RIST services
stop_services() {
    echo "Stopping all RIST services (if they exist)..."
    
    # Stop any running channel services
    if systemctl list-units --full --all | grep -q "rist-channel-"; then
        for service in $(systemctl list-units --full --all | grep "rist-channel-" | awk '{print $1}'); do
            echo "Stopping $service"
            systemctl stop "$service" || echo "Service $service was not running"
        done
    else
        echo "No rist-channel services found"
    fi
    
    # Stop transcoder services
    if systemctl list-units --full --all | grep -q "transcoder-"; then
        for service in $(systemctl list-units --full --all | grep "transcoder-" | awk '{print $1}'); do
            echo "Stopping $service"
            systemctl stop "$service" || echo "Service $service was not running"
        done
    else
        echo "No transcoder services found"
    fi
    
    # Stop main gateway service if it exists
    if systemctl list-unit-files | grep -q "rist-gateway"; then
        echo "Stopping rist-gateway service"
        systemctl stop rist-gateway || echo "rist-gateway service was not running"
    else
        echo "No rist-gateway service found"
    fi

    # Stop failover service if it exists
    if systemctl list-unit-files | grep -q "rist-failover"; then
        echo "Stopping rist-failover service"
        systemctl stop rist-failover || echo "rist-failover service was not running"
    else
        echo "No rist-failover service found"
    fi
    
    # Kill any remaining processes
    echo "Checking for remaining processes..."
    pkill -9 ffmpeg 2>/dev/null || true
    pkill -9 ffprobe 2>/dev/null || true
    pkill -9 ristreceiver 2>/dev/null || true
    pkill -9 ristsender 2>/dev/null || true
    pkill -9 basic_transcoder 2>/dev/null || true
    pkill -9 hls_server 2>/dev/null || true
    
    # Wait a moment for processes to clean up
    sleep 2
}

# Kill processes using a directory
kill_processes_using_directory() {
    local dir="$1"
    echo "Finding processes using $dir..."
    
    # Find processes using the directory
    lsof "$dir" | awk '{print $2}' | grep -v PID | while read pid; do
        if [ ! -z "$pid" ]; then
            echo "Killing process $pid using $dir"
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    
    # Wait a moment for processes to die
    sleep 2
}

# Set up web directory
setup_web() {
    echo "Setting up web directory..."
    
    # Stop services before unmounting
    stop_services
    
    # Unmount content directory if it's mounted
    if mountpoint -q /var/www/html/content; then
        echo "Unmounting content directory..."
        # Try to find and kill processes using the directory
        kill_processes_using_directory "/var/www/html/content"
        
        # Try normal unmount
        umount /var/www/html/content 2>/dev/null || {
            echo "Normal unmount failed, trying force unmount..."
            umount -f /var/www/html/content 2>/dev/null || {
                echo "Force unmount failed, trying lazy unmount..."
                umount -l /var/www/html/content 2>/dev/null || {
                    echo "WARNING: Could not unmount directory. Continuing anyway..."
                }
            }
        }
    fi
    
    echo "Clearing web directory..."
    rm -rf /var/www/html/*
    
    echo "Copying web files..."
    cp -r /root/ristgateway/web/* /var/www/html/
    
    # Create content directory if it doesn't exist
    echo "Creating content directory..."
    mkdir -p /var/www/html/content
    
    # Set permissions
    echo "Setting web permissions..."
    chmod -R 777 /var/www/html
}

# Configure tmpfs mount
setup_tmpfs() {
    echo "Setting up tmpfs mount..."
    
    # Unmount if already mounted
    if mountpoint -q /var/www/html/content; then
        umount /var/www/html/content
    fi
    
    # Mount tmpfs
    mount -t tmpfs -o size=2G tmpfs /var/www/html/content
    
    # Add to fstab if not already present
    if ! grep -q "/var/www/html/content" /etc/fstab; then
        echo "tmpfs   /var/www/html/content   tmpfs   defaults,size=2G   0   0" >> /etc/fstab
    fi
}

# Verify config
verify_config() {
    echo "Verifying configuration..."
    # Check for any required config files
    if [ ! -f "/root/ristgateway/receiver_config.yaml" ] && [ ! -f "/root/ristgateway/sender_config.yaml" ]; then
        echo "WARNING: No config files found. You may need to create them."
        # Create default configs if needed
        cat > /root/ristgateway/receiver_config.yaml << 'EOF'
channels: {}
EOF
        cat > /root/ristgateway/sender_config.yaml << 'EOF'
channels: {}
EOF
        cat > /root/ristgateway/transcoder_config.yaml << 'EOF'
transcoders: {}
EOF
    fi
}

# Set up API service
setup_service() {
    echo "Setting up systemd services..."
    
    echo "Creating log directory..."
    mkdir -p /var/log/ristgateway
    chmod 755 /var/log/ristgateway
    
    # Create main gateway service
    cat > /etc/systemd/system/rist-gateway.service << 'EOF'
[Unit]
Description=RIST Gateway API Service
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/root/ristgateway
ExecStart=/usr/bin/python3 -m uvicorn gateway_api:app --host 0.0.0.0 --port 5000
Restart=always
RestartSec=10
StandardOutput=append:/var/log/ristgateway/gateway.log
StandardError=append:/var/log/ristgateway/gateway-error.log

[Install]
WantedBy=multi-user.target
EOF

    # Create failover service if it exists
    if [ -f "/root/ristgateway/rist-failover.py" ]; then
        cat > /etc/systemd/system/rist-failover.service << 'EOF'
[Unit]
Description=RIST Failover Service
After=network.target rist-gateway.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/ristgateway
ExecStart=/usr/bin/python3 /root/ristgateway/rist-failover.py
Restart=always
RestartSec=10
StandardOutput=append:/var/log/ristgateway/failover.log
StandardError=append:/var/log/ristgateway/failover-error.log

[Install]
WantedBy=multi-user.target
EOF
    fi
    
    echo "Reloading systemd..."
    systemctl daemon-reload
    systemctl enable rist-gateway.service
    
    if [ -f "/etc/systemd/system/rist-failover.service" ]; then
        systemctl enable rist-failover.service
    fi
}

# Install and configure WireGuard
install_wireguard() {
    echo "Installing WireGuard..."
    apt-get install -y wireguard
    
    echo "Setting up WireGuard configuration..."
    mkdir -p /etc/wireguard
    
    # Create a template WireGuard configuration
    cat > /etc/wireguard/wg0.conf << 'EOF'
# WireGuard configuration template
# Replace with your actual configuration
[Interface]
PrivateKey = YOUR_PRIVATE_KEY
Address = YOUR_IP_ADDRESS/24
ListenPort = 51820

[Peer]
PublicKey = PEER_PUBLIC_KEY
AllowedIPs = 0.0.0.0/0
Endpoint = PEER_ENDPOINT:51820
EOF
    
    # Set proper permissions
    chmod 600 /etc/wireguard/wg0.conf
    
    echo ""
    echo "=========================================================="
    echo "WireGuard has been installed with a template configuration."
    echo "You need to edit the configuration file before using it:"
    echo ""
    echo "   sudo nano /etc/wireguard/wg0.conf"
    echo ""
    echo "After editing, enable and start WireGuard with:"
    echo "   sudo systemctl enable wg-quick@wg0"
    echo "   sudo systemctl start wg-quick@wg0"
    echo "=========================================================="
    echo ""
}

# Start services
start_services() {
    echo "Starting services..."
    echo "Starting nginx and redis..."
    systemctl restart nginx
    systemctl restart redis-server
    
    echo "Starting rist-gateway service..."
    systemctl start rist-gateway
    echo "Checking service status..."
    systemctl status rist-gateway --no-pager
    
    if [ -f "/etc/systemd/system/rist-failover.service" ]; then
        echo "Starting rist-failover service..."
        systemctl start rist-failover
        echo "Checking service status..."
        systemctl status rist-failover --no-pager
    fi
}

# Main installation flow
main() {
    check_root
    install_dependencies
    install_python_deps
    clone_repo
    verify_config
    setup_web
    setup_tmpfs
    setup_service
    install_wireguard
    start_services
    
    echo "Installation completed successfully!"
    echo "Please check service status with: systemctl status rist-gateway"
    echo ""
    echo "Web interface should be available at: http://YOUR_SERVER_IP/"
    echo "API endpoint: http://YOUR_SERVER_IP:5000/"
    echo ""
    echo "IMPORTANT: Don't forget to edit your WireGuard configuration"
    echo "and start the service as mentioned in the instructions above."
    echo ""
    echo "=========================================================="
    echo "Next steps:"
    echo "1. Configure WireGuard: sudo nano /etc/wireguard/wg0.conf"
    echo "2. Start WireGuard: sudo systemctl enable --now wg-quick@wg0"
    echo "3. Access the web interface to configure channels"
    echo "=========================================================="
    echo ""
}

# Run main installation
main
