const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');

class MouffetteServer {
    constructor(port = 8080) {
        this.port = port;
        this.clients = new Map(); // clientId -> client info
        this.wss = null;
    }

    start() {
        // Bind explicitly to 0.0.0.0 to listen on all IPv4 interfaces (LAN accessible)
        this.wss = new WebSocket.Server({ port: this.port, host: '0.0.0.0' });
        
        console.log(`🎯 Mouffette Server started on ws://0.0.0.0:${this.port}`);
        
        this.wss.on('connection', (ws, req) => {
            const clientId = uuidv4();
            const clientInfo = {
                id: clientId,
                ws: ws,
                machineName: null,
                screens: [],
                status: 'connected',
                connectedAt: new Date().toISOString()
            };
            
            this.clients.set(clientId, clientInfo);
            console.log(`📱 New client connected: ${clientId}`);
            
            // Send welcome message with client ID
            ws.send(JSON.stringify({
                type: 'welcome',
                clientId: clientId,
                message: 'Connected to Mouffette Server'
            }));
            
            ws.on('message', (data) => {
                try {
                    const message = JSON.parse(data.toString());
                    this.handleMessage(clientId, message);
                } catch (error) {
                    console.error('❌ Error parsing message:', error);
                    ws.send(JSON.stringify({
                        type: 'error',
                        message: 'Invalid JSON format'
                    }));
                }
            });
            
            ws.on('close', () => {
                console.log(`📱 Client disconnected: ${clientId}`);
                this.clients.delete(clientId);
                this.broadcastClientList();
            });
            
            ws.on('error', (error) => {
                console.error(`❌ WebSocket error for client ${clientId}:`, error);
                this.clients.delete(clientId);
                this.broadcastClientList();
            });
            
            // Send current client list to new client
            this.sendClientList(clientId);
            // Broadcast updated client list to all clients
            this.broadcastClientList();
        });
    }
    
    handleMessage(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
        
        console.log(`📨 Message from ${clientId}:`, message.type);
        
        switch (message.type) {
            case 'register':
                this.handleRegister(clientId, message);
                break;
            case 'request_client_list':
                this.sendClientList(clientId);
                break;
            case 'media_share':
                this.handleMediaShare(clientId, message);
                break;
            case 'media_update':
                this.handleMediaUpdate(clientId, message);
                break;
            case 'stop_sharing':
                this.handleStopSharing(clientId, message);
                break;
            default:
                console.log(`⚠️ Unknown message type: ${message.type}`);
        }
    }
    
    handleRegister(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
        
        client.machineName = message.machineName || `Client-${clientId.slice(0, 8)}`;
        client.screens = message.screens || [];
        client.platform = message.platform || 'unknown';
        
        console.log(`✅ Client registered: ${client.machineName} (${client.platform}) with ${client.screens.length} screen(s)`);
        
        // Send confirmation
        client.ws.send(JSON.stringify({
            type: 'registration_confirmed',
            clientInfo: {
                id: clientId,
                machineName: client.machineName,
                screens: client.screens,
                platform: client.platform
            }
        }));
        
        // Broadcast updated client list
        this.broadcastClientList();
    }
    
    sendClientList(clientId) {
        const client = this.clients.get(clientId);
        if (!client) return;
        
        const clientList = Array.from(this.clients.values())
            .filter(c => c.id !== clientId && c.machineName) // Don't include self and only registered clients
            .map(c => ({
                id: c.id,
                machineName: c.machineName,
                screens: c.screens,
                platform: c.platform,
                status: c.status
            }));
        
        client.ws.send(JSON.stringify({
            type: 'client_list',
            clients: clientList
        }));
    }
    
    broadcastClientList() {
        for (const [clientId, client] of this.clients) {
            if (client.machineName) { // Only send to registered clients
                this.sendClientList(clientId);
            }
        }
    }
    
    handleMediaShare(senderId, message) {
        const targetClient = this.clients.get(message.targetClientId);
        if (!targetClient) {
            const senderClient = this.clients.get(senderId);
            if (senderClient) {
                senderClient.ws.send(JSON.stringify({
                    type: 'error',
                    message: 'Target client not found'
                }));
            }
            return;
        }
        
        console.log(`🎬 Media share from ${senderId} to ${message.targetClientId}`);
        
        // Forward media share to target client
        targetClient.ws.send(JSON.stringify({
            type: 'incoming_media',
            senderId: senderId,
            mediaData: message.mediaData,
            screens: message.screens
        }));
        
        // Confirm to sender
        const senderClient = this.clients.get(senderId);
        if (senderClient) {
            senderClient.ws.send(JSON.stringify({
                type: 'share_initiated',
                targetClientId: message.targetClientId
            }));
        }
    }
    
    handleMediaUpdate(senderId, message) {
        const targetClient = this.clients.get(message.targetClientId);
        if (!targetClient) return;
        
        // Forward real-time updates to target client
        targetClient.ws.send(JSON.stringify({
            type: 'media_update',
            senderId: senderId,
            updates: message.updates
        }));
    }
    
    handleStopSharing(senderId, message) {
        const targetClient = this.clients.get(message.targetClientId);
        if (!targetClient) return;
        
        console.log(`🛑 Stop sharing from ${senderId} to ${message.targetClientId}`);
        
        // Tell target client to stop displaying media
        targetClient.ws.send(JSON.stringify({
            type: 'stop_media',
            senderId: senderId
        }));
    }
    
    getStats() {
        return {
            connectedClients: this.clients.size,
            registeredClients: Array.from(this.clients.values()).filter(c => c.machineName).length
        };
    }
}

// Start the server
const server = new MouffetteServer(8080);
server.start();

// Display stats every 30 seconds
setInterval(() => {
    const stats = server.getStats();
    console.log(`📊 Stats: ${stats.connectedClients} connected, ${stats.registeredClients} registered`);
}, 30000);

// Graceful shutdown
process.on('SIGINT', () => {
    console.log('\n🛑 Shutting down Mouffette Server...');
    if (server.wss) {
        server.wss.close(() => {
            console.log('✅ Server closed gracefully');
            process.exit(0);
        });
    } else {
        process.exit(0);
    }
});
