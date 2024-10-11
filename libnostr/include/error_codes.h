#ifndef NOSTR_ERROR_CODES_H
#define NOSTR_ERROR_CODES_H

// General errors
#define ERR_SUCCESS 0                 // No error, operation successful
#define ERR_UNKNOWN 1                 // Unknown error occurred

// Memory-related errors
#define ERR_MEMORY_ALLOCATION_FAILED 100 // Memory allocation failed
#define ERR_NULL_POINTER 101             // Null pointer encountered

// WebSocket-related errors
#define ERR_WEBSOCKET_CONNECTION_FAILED 200 // Failed to establish WebSocket connection
#define ERR_WEBSOCKET_WRITE_FAILED 201      // Failed to write to WebSocket
#define ERR_WEBSOCKET_READ_FAILED 202       // Failed to read from WebSocket
#define ERR_WEBSOCKET_HANDSHAKE_FAILED 203  // WebSocket handshake failed
#define ERR_WEBSOCKET_CLOSED 204            // WebSocket connection closed
#define ERR_WEBSOCKET_INVALID_FRAME 205     // Invalid WebSocket frame received

// HTTP-related errors
#define ERR_HTTP_REQUEST_FAILED 300       // HTTP request failed
#define ERR_HTTP_INVALID_RESPONSE 301     // Invalid HTTP response
#define ERR_HTTP_HEADER_MISSING 302       // Missing required HTTP headers

// SSL/TLS-related errors
#define ERR_SSL_INITIALIZATION_FAILED 400 // Failed to initialize SSL/TLS
#define ERR_SSL_CONNECTION_FAILED 401     // Failed to establish SSL/TLS connection
#define ERR_SSL_HANDSHAKE_FAILED 402      // SSL/TLS handshake failed

// JSON-related errors
#define ERR_JSON_PARSE_FAILED 500       // JSON parsing failed
#define ERR_JSON_SERIALIZATION_FAILED 501 // JSON serialization failed
#define ERR_JSON_DESERIALIZATION_FAILED 502 // JSON deserialization failed

// Relay-related errors
#define ERR_RELAY_INVALID_URL 600         // Invalid relay URL
#define ERR_RELAY_CONNECTION_FAILED 601   // Failed to connect to relay
#define ERR_RELAY_WRITE_FAILED 602        // Failed to write to relay
#define ERR_RELAY_CLOSE_FAILED 603        // Failed to close relay connection
#define ERR_RELAY_SUBSCRIBE_FAILED 604    // Subscription to relay failed
#define ERR_RELAY_AUTH_FAILED 605         // Relay authentication failed
#define ERR_RELAY_PUBLISH_FAILED 606      // Publishing event to relay failed

// Subscription-related errors
#define ERR_SUBSCRIPTION_INIT_FAILED 700  // Failed to initialize subscription
#define ERR_SUBSCRIPTION_CLOSE_FAILED 701 // Failed to close subscription
#define ERR_SUBSCRIPTION_UNSUB_FAILED 702 // Failed to unsubscribe

// Event-related errors
#define ERR_EVENT_SIGNATURE_INVALID 800   // Event signature invalid
#define ERR_EVENT_SERIALIZATION_FAILED 801 // Event serialization failed
#define ERR_EVENT_DESERIALIZATION_FAILED 802 // Event deserialization failed

// Channel-related errors
#define ERR_CHANNEL_CREATION_FAILED 900  // Failed to create Go-style channel
#define ERR_CHANNEL_SEND_FAILED 901      // Failed to send data to channel
#define ERR_CHANNEL_RECEIVE_FAILED 902   // Failed to receive data from channel

// Context-related errors
#define ERR_CONTEXT_CANCELED 1000        // Context canceled
#define ERR_CONTEXT_TIMEOUT 1001         // Context timeout occurred

// Filter-related errors
#define ERR_FILTER_PARSE_FAILED 1100     // Failed to parse filter
#define ERR_FILTER_SERIALIZATION_FAILED 1101 // Failed to serialize filter
#define ERR_FILTER_DESERIALIZATION_FAILED 1102 // Failed to deserialize filter

// Miscellaneous errors
#define ERR_INVALID_ARGUMENT 1200        // Invalid argument passed to function
#define ERR_OPERATION_NOT_SUPPORTED 1201 // Operation not supported
#define ERR_INTERNAL_ERROR 1202          // Internal error occurred

#endif // NOSTR_ERROR_CODES_H
