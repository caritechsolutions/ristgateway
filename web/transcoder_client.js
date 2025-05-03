/**
 * Enhanced Transcoder API Client
 * 
 * This module provides functions to interact with the updated transcoder API endpoints
 * with support for the new transcoder parameters
 */

class TranscoderClient {
    constructor(baseUrl = '') {
        this.API_BASE = baseUrl || `${window.location.protocol}//${window.location.hostname}:5000`;
        this.TRANSCODERS_BASE = `${this.API_BASE}/transcoders`;
    }

    async handleToggleTranscoder(transcoderId) {
        try {
            // Get transcoder info
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}`);
            if (!response.ok) throw new Error('Failed to get transcoder info');
            const transcoder = await response.json();

            const isRunning = transcoder.status === 'running';
            const action = isRunning ? 'stop' : 'start';

            // Find the button and update its appearance
            const row = document.querySelector(`tr[data-channel-id="${transcoderId}"]`);
            if (!row) {
                console.error(`Could not find row for transcoder ${transcoderId}`);
                return;
            }
            
            // Get all buttons in the row
            const buttons = row.querySelectorAll('button');
            
            // Find the action button (Start or Stop)
            const actionButton = Array.from(buttons).find(btn => {
                return (!isRunning && btn.textContent.trim() === 'Start') || 
                       (isRunning && btn.textContent.trim() === 'Stop');
            });
            
            if (!actionButton) {
                console.error(`Could not find ${isRunning ? 'Stop' : 'Start'} button for transcoder ${transcoderId}`);
                return;
            }
            
            // Disable the button and show animation
            if (actionButton) {
                actionButton.disabled = true;
                actionButton.textContent = isRunning ? 'Stopping...' : 'Starting...';
            }
            
            // Call the API
            const actionResponse = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}/${action}`, {
                method: 'PUT'
            });
            
            if (!actionResponse.ok) {
                const errorData = await actionResponse.json();
                throw new Error(errorData.detail || `Failed to ${action} transcoder`);
            }
            
            // Wait a bit for the operation to take effect
            await new Promise(resolve => setTimeout(resolve, 2000));
            
            // Refresh the channel list
            if (typeof channelManager !== 'undefined' && channelManager.loadChannels) {
                await channelManager.loadChannels();
            }
            
        } catch (error) {
            console.error(`Error toggling transcoder ${transcoderId}:`, error);
            alert(`Error ${isRunning ? 'stopping' : 'starting'} transcoder: ${error.message}`);
            
            // Reset button state on error
            const row = document.querySelector(`tr[data-channel-id="${transcoderId}"]`);
            if (row) {
                const buttons = row.querySelectorAll('button');
                const actionButton = Array.from(buttons).find(btn => {
                    return btn.textContent.includes('Starting') || btn.textContent.includes('Stopping');
                });
                
                if (actionButton) {
                    actionButton.disabled = false;
                    actionButton.textContent = isRunning ? 'Stop' : 'Start';
                }
            }
        }
    }

    async handleDeleteTranscoder(transcoderId) {
        if (!confirm(`Are you sure you want to delete this transcoder?`)) return;
        
        try {
            const response = await this.deleteTranscoder(transcoderId);
            
            // Refresh the channel list
            if (typeof channelManager !== 'undefined' && channelManager.loadChannels) {
                await channelManager.loadChannels();
            }
            
            return response;
        } catch (error) {
            console.error(`Error deleting transcoder ${transcoderId}:`, error);
            alert(`Error deleting transcoder: ${error.message}`);
            throw error;
        }
    }

    /**
     * Get available encoding devices
     * @returns {Promise<Array>} - List of available devices
     */
    async getAvailableDevices() {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/available-devices`);
            if (!response.ok) {
                throw new Error(`Failed to get available devices: ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error('Error fetching available devices:', error);
            return [{ id: 'cpu', name: 'CPU', type: 'cpu' }]; // Default fallback
        }
    }

    /**
     * Get all transcoders
     * @returns {Promise<Object>} - Map of transcoders by ID
     */
    async getAllTranscoders() {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}`);
            if (!response.ok) {
                throw new Error(`Failed to get transcoders: ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error('Error fetching transcoders:', error);
            throw error;
        }
    }

    /**
     * Get a specific transcoder by ID
     * @param {string} transcoderId - The ID of the transcoder
     * @returns {Promise<Object>} - Transcoder details
     */
    async getTranscoder(transcoderId) {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}`);
            if (!response.ok) {
                throw new Error(`Failed to get transcoder: ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error(`Error fetching transcoder ${transcoderId}:`, error);
            throw error;
        }
    }

    /**
     * Create a new transcoder
     * @param {string} transcoderId - The ID for the new transcoder
     * @param {Object} transcoderData - The transcoder configuration
     * @returns {Promise<Object>} - Created transcoder details
     */
    async createTranscoder(transcoderId, transcoderData) {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}`, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(transcoderData)
            });

            if (!response.ok) {
                const errorData = await response.json();
                throw new Error(errorData.detail || `Failed to create transcoder: ${response.status}`);
            }

            return await response.json();
        } catch (error) {
            console.error('Error creating transcoder:', error);
            throw error;
        }
    }

    /**
     * Update an existing transcoder
     * @param {string} transcoderId - The ID of the transcoder to update
     * @param {Object} transcoderData - The updated transcoder configuration
     * @returns {Promise<Object>} - Updated transcoder details
     */
    async updateTranscoder(transcoderId, transcoderData) {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}`, {
                method: 'PUT',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(transcoderData)
            });

            if (!response.ok) {
                const errorData = await response.json();
                throw new Error(errorData.detail || `Failed to update transcoder: ${response.status}`);
            }

            return await response.json();
        } catch (error) {
            console.error(`Error updating transcoder ${transcoderId}:`, error);
            throw error;
        }
    }

    /**
     * Delete a transcoder
     * @param {string} transcoderId - The ID of the transcoder to delete
     * @returns {Promise<Object>} - Deletion status
     */
    async deleteTranscoder(transcoderId) {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}`, {
                method: 'DELETE'
            });

            if (!response.ok) {
                const errorData = await response.json();
                throw new Error(errorData.detail || `Failed to delete transcoder: ${response.status}`);
            }

            return await response.json();
        } catch (error) {
            console.error(`Error deleting transcoder ${transcoderId}:`, error);
            throw error;
        }
    }

    /**
     * Start a transcoder
     * @param {string} transcoderId - The ID of the transcoder to start
     * @returns {Promise<Object>} - Start status
     */
    async startTranscoder(transcoderId) {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}/start`, {
                method: 'PUT'
            });

            if (!response.ok) {
                const errorData = await response.json();
                throw new Error(errorData.detail || `Failed to start transcoder: ${response.status}`);
            }

            return await response.json();
        } catch (error) {
            console.error(`Error starting transcoder ${transcoderId}:`, error);
            throw error;
        }
    }

    /**
     * Stop a transcoder
     * @param {string} transcoderId - The ID of the transcoder to stop
     * @returns {Promise<Object>} - Stop status
     */
    async stopTranscoder(transcoderId) {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}/stop`, {
                method: 'PUT'
            });

            if (!response.ok) {
                const errorData = await response.json();
                throw new Error(errorData.detail || `Failed to stop transcoder: ${response.status}`);
            }

            return await response.json();
        } catch (error) {
            console.error(`Error stopping transcoder ${transcoderId}:`, error);
            throw error;
        }
    }

    /**
     * Get metrics for a transcoder
     * @param {string} transcoderId - The ID of the transcoder
     * @returns {Promise<Object>} - Transcoder metrics
     */
    async getTranscoderMetrics(transcoderId) {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}/metrics`);
            if (!response.ok) {
                throw new Error(`Failed to get metrics: ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error(`Error fetching metrics for transcoder ${transcoderId}:`, error);
            throw error;
        }
    }

    /**
     * Get buffer stats for a transcoder
     * @param {string} transcoderId - The ID of the transcoder
     * @returns {Promise<Object>} - Transcoder buffer statistics
     */
    async getTranscoderBuffers(transcoderId) {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/${transcoderId}/buffers`);
            if (!response.ok) {
                throw new Error(`Failed to get buffer stats: ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error(`Error fetching buffer stats for transcoder ${transcoderId}:`, error);
            throw error;
        }
    }

    /**
     * Get information for the next transcoder ID
     * @returns {Promise<Object>} - Next transcoder ID info with metrics port
     */
    async getNextTranscoderId() {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/next`);
            if (!response.ok) {
                throw new Error(`Failed to get next transcoder ID: ${response.status}`);
            }
            const data = await response.json();
            // Now includes both transcoder_id and metrics_port
            return data;
        } catch (error) {
            console.error('Error fetching next transcoder ID:', error);
            throw error;
        }
    }

    /**
     * Get all available GStreamer plugins
     * @returns {Promise<Array>} - List of available GStreamer plugins
     */
    async getGstreamerPlugins() {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/gstreamer/plugins`);
            if (!response.ok) {
                throw new Error(`Failed to get GStreamer plugins: ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error('Error fetching GStreamer plugins:', error);
            throw error;
        }
    }

    /**
     * Get status summary of all transcoders
     * @returns {Promise<Object>} - Transcoder status summary
     */
    async getTranscodersStatus() {
        try {
            const response = await fetch(`${this.TRANSCODERS_BASE}/status`);
            if (!response.ok) {
                throw new Error(`Failed to get transcoders status: ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error('Error fetching transcoders status:', error);
            throw error;
        }
    }

    /**
     * Format transcoder form data into API structure
     * @param {Object} formData - Form data from the transcoder modal
     * @returns {Object} - Structured transcoder data for API
     */
    formatTranscoderData(formData) {
        return {
            name: formData.transcoderName,
            input: {
                address: formData.inputAddress,
                program_pid: formData.programPid ? parseInt(formData.programPid) : null,
                video_pid: formData.videoPid ? parseInt(formData.videoPid) : null,
                audio_pid: formData.audioPid ? parseInt(formData.audioPid) : null
            },
            video: {
                device: formData.videoDevice,
                codec: formData.videoCodec,
                resolution: formData.videoResolution,
                b_frames: parseInt(formData.bFrames || 0),
                profile: formData.videoProfile,
                preset: formData.videoPreset,
                deinterlace: formData.deinterlace,
                bitrate: parseInt(formData.videoBitrate || 0),
                keyframe_interval: parseInt(formData.keyframeInterval || 60)
            },
            audio: {
                codec: formData.audioCodec,
                sample_rate: formData.sampleRate,
                channels: formData.audioChannels,
                bitrate: parseInt(formData.audioBitrate || 0)
            },
            output: {
                address: formData.outputAddress,
                program_pid: formData.outputProgramPid ? parseInt(formData.outputProgramPid) : null,
                video_pid: formData.outputVideoPid ? parseInt(formData.outputVideoPid) : null,
                audio_pid: formData.outputAudioPid ? parseInt(formData.outputAudioPid) : null,
                mux_bitrate: formData.muxBitrate ? parseInt(formData.muxBitrate) : null
            },
            buffer_settings: {
                buffer_mode: parseInt(formData.bufferMode || 0),
                leaky_mode: parseInt(formData.leakyMode || 0),
                buffer_size_mb: parseInt(formData.bufferSize || 4),
                buffer_time_ms: parseInt(formData.bufferTime || 500)
            },
            advanced_settings: {
                watchdog_enabled: formData.watchdogEnabled || false,
                watchdog_timeout: parseInt(formData.watchdogTimeout || 10),
                add_clock_overlay: formData.addClockOverlay || false
            },
            metrics_port: formData.metricsPort ? parseInt(formData.metricsPort) : null,
            enabled: true,
            status: "stopped"
        };
    }
}

// Export as a global or module
if (typeof window !== 'undefined') {
    window.TranscoderClient = TranscoderClient;
}
