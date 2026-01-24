/**
 * NIP-90: Data Vending Machines (DVM)
 *
 * NIP-90 defines a protocol for "data vending machines" that process
 * jobs for payment. DVMs are services that accept job requests and
 * return results, typically for a fee paid via Lightning.
 *
 * Event Kinds:
 * - 5000-5999: Job requests (kind = 5000 + job type)
 * - 6000-6999: Job results (kind = 6000 + job type)
 * - 7000: Job feedback (status updates during processing)
 *
 * Common Job Types:
 * - 5000/6000: Text translation
 * - 5001/6001: Text summarization
 * - 5002/6002: Image generation
 * - 5003/6003: Text-to-speech
 * - 5004/6004: Speech-to-text
 * - 5005/6005: Content discovery/recommendation
 *
 * Request Tags:
 * - ["i", "<input-data>", "<input-type>", "<relay>", "<marker>"] - input data
 * - ["output", "<mime-type>"] - expected output format
 * - ["bid", "<msats>", "<max-msats>"] - payment bid
 * - ["relays", "relay1", ...] - relays for response
 * - ["p", "<pubkey>"] - target service provider
 * - ["param", "<name>", "<value>"] - job-specific parameters
 *
 * Result Tags:
 * - ["request", "<event-json>"] - original request
 * - ["e", "<request-id>", "<relay>"] - reference to request
 * - ["i", ...] - same as request input
 * - ["amount", "<msats>", "<bolt11>"] - payment request
 *
 * Feedback Tags (kind 7000):
 * - ["status", "<status>", "<extra-info>"] - processing, error, success, partial
 * - ["amount", "<msats>", "<bolt11>"] - payment request
 * - ["e", "<request-id>"] - reference to request
 * - ["p", "<requester-pubkey>"] - reference to requester
 */

#ifndef GNOSTR_NIP90_DVM_H
#define GNOSTR_NIP90_DVM_H

#include <glib.h>

G_BEGIN_DECLS

/* ============== Event Kind Constants ============== */

/**
 * Job Request Kinds: 5000-5999
 * The specific kind is 5000 + job_type
 */
#define GNOSTR_DVM_KIND_REQUEST_MIN        5000
#define GNOSTR_DVM_KIND_REQUEST_MAX        5999

/**
 * Job Result Kinds: 6000-6999
 * The specific kind is 6000 + job_type (matches request kind - 5000 + 6000)
 */
#define GNOSTR_DVM_KIND_RESULT_MIN         6000
#define GNOSTR_DVM_KIND_RESULT_MAX         6999

/**
 * Job Feedback Kind: 7000
 * Used for status updates during job processing
 */
#define GNOSTR_DVM_KIND_FEEDBACK           7000

/* Common job type offsets (add to 5000 for request, 6000 for result) */
#define GNOSTR_DVM_JOB_TEXT_TRANSLATION    0    /* 5000/6000 */
#define GNOSTR_DVM_JOB_TEXT_SUMMARIZATION  1    /* 5001/6001 */
#define GNOSTR_DVM_JOB_IMAGE_GENERATION    2    /* 5002/6002 */
#define GNOSTR_DVM_JOB_TEXT_TO_SPEECH      3    /* 5003/6003 */
#define GNOSTR_DVM_JOB_SPEECH_TO_TEXT      4    /* 5004/6004 */
#define GNOSTR_DVM_JOB_CONTENT_DISCOVERY   5    /* 5005/6005 */

/* ============== Input Types ============== */

/**
 * GnostrDvmInputType:
 * @GNOSTR_DVM_INPUT_TEXT: Plain text input
 * @GNOSTR_DVM_INPUT_URL: URL to fetch content from
 * @GNOSTR_DVM_INPUT_EVENT: Nostr event ID (hex)
 * @GNOSTR_DVM_INPUT_JOB: Reference to another DVM job result
 *
 * Input data types for DVM job requests
 */
typedef enum {
  GNOSTR_DVM_INPUT_UNKNOWN = 0,
  GNOSTR_DVM_INPUT_TEXT,
  GNOSTR_DVM_INPUT_URL,
  GNOSTR_DVM_INPUT_EVENT,
  GNOSTR_DVM_INPUT_JOB
} GnostrDvmInputType;

/* ============== Job Status ============== */

/**
 * GnostrDvmJobStatus:
 * @GNOSTR_DVM_STATUS_UNKNOWN: Unknown or unrecognized status
 * @GNOSTR_DVM_STATUS_PROCESSING: Job is being processed
 * @GNOSTR_DVM_STATUS_ERROR: Job failed with an error
 * @GNOSTR_DVM_STATUS_SUCCESS: Job completed successfully
 * @GNOSTR_DVM_STATUS_PARTIAL: Partial results available
 * @GNOSTR_DVM_STATUS_PAYMENT_REQUIRED: Payment needed before processing
 *
 * Status values for DVM job feedback
 */
typedef enum {
  GNOSTR_DVM_STATUS_UNKNOWN = 0,
  GNOSTR_DVM_STATUS_PROCESSING,
  GNOSTR_DVM_STATUS_ERROR,
  GNOSTR_DVM_STATUS_SUCCESS,
  GNOSTR_DVM_STATUS_PARTIAL,
  GNOSTR_DVM_STATUS_PAYMENT_REQUIRED
} GnostrDvmJobStatus;

/* ============== Data Structures ============== */

/**
 * GnostrDvmInput:
 * Single input item for a DVM job request
 */
typedef struct {
  gchar *data;                    /* Input data (text, URL, event ID, etc.) */
  GnostrDvmInputType type;        /* Type of input */
  gchar *relay;                   /* Optional relay hint for event inputs */
  gchar *marker;                  /* Optional marker/label for this input */
} GnostrDvmInput;

/**
 * GnostrDvmParam:
 * Job-specific parameter
 */
typedef struct {
  gchar *name;                    /* Parameter name */
  gchar *value;                   /* Parameter value */
} GnostrDvmParam;

/**
 * GnostrDvmJobRequest:
 * Structure representing a DVM job request
 */
typedef struct {
  /* Event metadata */
  gchar *event_id;                /* Event ID (hex) after publishing */
  gchar *pubkey;                  /* Requester's public key (hex) */
  gint64 created_at;              /* Timestamp */
  gint job_type;                  /* Job type (0-999, added to 5000 for kind) */

  /* Inputs */
  GnostrDvmInput *inputs;         /* Array of input items */
  gsize n_inputs;                 /* Number of inputs */

  /* Output specification */
  gchar *output_mime;             /* Expected output MIME type */

  /* Payment bid */
  gint64 bid_msats;               /* Minimum bid in millisatoshis */
  gint64 max_msats;               /* Maximum willing to pay (0 = unlimited) */

  /* Target and relays */
  gchar *target_pubkey;           /* Optional: specific DVM service provider */
  gchar **relays;                 /* NULL-terminated array of relay URLs */
  gsize n_relays;                 /* Number of relays */

  /* Additional parameters */
  GnostrDvmParam *params;         /* Job-specific parameters */
  gsize n_params;                 /* Number of parameters */

  /* Encrypted flag */
  gboolean encrypted;             /* Whether request is NIP-04/NIP-44 encrypted */
} GnostrDvmJobRequest;

/**
 * GnostrDvmJobResult:
 * Structure representing a DVM job result
 */
typedef struct {
  /* Event metadata */
  gchar *event_id;                /* Result event ID (hex) */
  gchar *pubkey;                  /* DVM service provider pubkey (hex) */
  gint64 created_at;              /* Timestamp */
  gint job_type;                  /* Job type (0-999, added to 6000 for kind) */

  /* References */
  gchar *request_id;              /* Original request event ID */
  gchar *request_relay;           /* Relay where request was found */
  gchar *requester_pubkey;        /* Pubkey of the original requester */

  /* Result content */
  gchar *content;                 /* Result content (from event content) */
  GnostrDvmJobStatus status;      /* Job status */

  /* Payment info */
  gint64 amount_msats;            /* Amount charged in millisatoshis */
  gchar *bolt11;                  /* Lightning invoice for payment */

  /* Encrypted flag */
  gboolean encrypted;             /* Whether result is encrypted */
} GnostrDvmJobResult;

/**
 * GnostrDvmJobFeedback:
 * Structure representing job feedback (kind 7000)
 */
typedef struct {
  /* Event metadata */
  gchar *event_id;                /* Feedback event ID (hex) */
  gchar *pubkey;                  /* DVM service provider pubkey (hex) */
  gint64 created_at;              /* Timestamp */

  /* References */
  gchar *request_id;              /* Original request event ID */
  gchar *requester_pubkey;        /* Pubkey of the original requester */

  /* Status */
  GnostrDvmJobStatus status;      /* Current job status */
  gchar *status_extra;            /* Additional status info (error message, etc.) */

  /* Payment info (for payment-required status) */
  gint64 amount_msats;            /* Amount required */
  gchar *bolt11;                  /* Lightning invoice */

  /* Progress (optional) */
  gint progress_percent;          /* 0-100, -1 if not available */

  /* Content (optional partial results) */
  gchar *content;                 /* Partial results or status message */
} GnostrDvmJobFeedback;

/* ============== Memory Management ============== */

/**
 * gnostr_dvm_input_free:
 * @input: Input to free (does not free the struct itself)
 *
 * Frees the contents of a DVM input structure.
 */
void gnostr_dvm_input_free(GnostrDvmInput *input);

/**
 * gnostr_dvm_param_free:
 * @param: Parameter to free (does not free the struct itself)
 *
 * Frees the contents of a DVM parameter structure.
 */
void gnostr_dvm_param_free(GnostrDvmParam *param);

/**
 * gnostr_dvm_job_request_new:
 * @job_type: Job type (0-999)
 *
 * Creates a new empty job request structure.
 *
 * Returns: (transfer full): New job request. Free with gnostr_dvm_job_request_free().
 */
GnostrDvmJobRequest *gnostr_dvm_job_request_new(gint job_type);

/**
 * gnostr_dvm_job_request_free:
 * @request: Request to free
 *
 * Frees all memory associated with a job request.
 */
void gnostr_dvm_job_request_free(GnostrDvmJobRequest *request);

/**
 * gnostr_dvm_job_result_free:
 * @result: Result to free
 *
 * Frees all memory associated with a job result.
 */
void gnostr_dvm_job_result_free(GnostrDvmJobResult *result);

/**
 * gnostr_dvm_job_feedback_free:
 * @feedback: Feedback to free
 *
 * Frees all memory associated with job feedback.
 */
void gnostr_dvm_job_feedback_free(GnostrDvmJobFeedback *feedback);

/* ============== Request Building ============== */

/**
 * gnostr_dvm_job_request_add_input:
 * @request: Job request
 * @data: Input data
 * @type: Input type
 * @relay: (nullable): Optional relay hint
 * @marker: (nullable): Optional marker
 *
 * Adds an input to the job request.
 */
void gnostr_dvm_job_request_add_input(GnostrDvmJobRequest *request,
                                       const gchar *data,
                                       GnostrDvmInputType type,
                                       const gchar *relay,
                                       const gchar *marker);

/**
 * gnostr_dvm_job_request_add_param:
 * @request: Job request
 * @name: Parameter name
 * @value: Parameter value
 *
 * Adds a parameter to the job request.
 */
void gnostr_dvm_job_request_add_param(GnostrDvmJobRequest *request,
                                       const gchar *name,
                                       const gchar *value);

/**
 * gnostr_dvm_job_request_add_relay:
 * @request: Job request
 * @relay_url: Relay URL
 *
 * Adds a relay to the job request.
 */
void gnostr_dvm_job_request_add_relay(GnostrDvmJobRequest *request,
                                       const gchar *relay_url);

/**
 * gnostr_dvm_build_request_tags:
 * @request: Job request
 *
 * Builds the tags array for a job request event as JSON.
 *
 * Returns: (transfer full): JSON array string of tags. Caller must g_free().
 */
gchar *gnostr_dvm_build_request_tags(const GnostrDvmJobRequest *request);

/**
 * gnostr_dvm_build_request_event:
 * @request: Job request
 *
 * Builds the complete unsigned job request event JSON.
 * The event must be signed before publishing.
 *
 * Returns: (transfer full) (nullable): JSON string of unsigned event. Caller must g_free().
 */
gchar *gnostr_dvm_build_request_event(const GnostrDvmJobRequest *request);

/* ============== Parsing ============== */

/**
 * gnostr_dvm_job_request_parse:
 * @json_str: JSON string of the request event
 *
 * Parses a job request event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed request or NULL on error.
 */
GnostrDvmJobRequest *gnostr_dvm_job_request_parse(const gchar *json_str);

/**
 * gnostr_dvm_job_result_parse:
 * @json_str: JSON string of the result event
 *
 * Parses a job result event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed result or NULL on error.
 */
GnostrDvmJobResult *gnostr_dvm_job_result_parse(const gchar *json_str);

/**
 * gnostr_dvm_job_feedback_parse:
 * @json_str: JSON string of the feedback event
 *
 * Parses a job feedback event (kind 7000) from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed feedback or NULL on error.
 */
GnostrDvmJobFeedback *gnostr_dvm_job_feedback_parse(const gchar *json_str);

/**
 * gnostr_dvm_parse_input_type:
 * @type_str: Input type string ("text", "url", "event", "job")
 *
 * Parses an input type string to enum.
 *
 * Returns: Input type enum value
 */
GnostrDvmInputType gnostr_dvm_parse_input_type(const gchar *type_str);

/**
 * gnostr_dvm_input_type_to_string:
 * @type: Input type enum
 *
 * Converts input type enum to string.
 *
 * Returns: (transfer none): Static string for the input type
 */
const gchar *gnostr_dvm_input_type_to_string(GnostrDvmInputType type);

/**
 * gnostr_dvm_parse_status:
 * @status_str: Status string ("processing", "error", "success", "partial", "payment-required")
 *
 * Parses a status string to enum.
 *
 * Returns: Status enum value
 */
GnostrDvmJobStatus gnostr_dvm_parse_status(const gchar *status_str);

/**
 * gnostr_dvm_status_to_string:
 * @status: Status enum
 *
 * Converts status enum to string.
 *
 * Returns: (transfer none): Static string for the status
 */
const gchar *gnostr_dvm_status_to_string(GnostrDvmJobStatus status);

/* ============== Kind Helpers ============== */

/**
 * gnostr_dvm_is_request_kind:
 * @kind: Event kind
 *
 * Checks if the kind is a DVM job request (5000-5999).
 *
 * Returns: TRUE if kind is a request kind
 */
gboolean gnostr_dvm_is_request_kind(gint kind);

/**
 * gnostr_dvm_is_result_kind:
 * @kind: Event kind
 *
 * Checks if the kind is a DVM job result (6000-6999).
 *
 * Returns: TRUE if kind is a result kind
 */
gboolean gnostr_dvm_is_result_kind(gint kind);

/**
 * gnostr_dvm_is_feedback_kind:
 * @kind: Event kind
 *
 * Checks if the kind is DVM job feedback (7000).
 *
 * Returns: TRUE if kind is feedback kind
 */
gboolean gnostr_dvm_is_feedback_kind(gint kind);

/**
 * gnostr_dvm_request_kind_for_job:
 * @job_type: Job type (0-999)
 *
 * Gets the request kind for a job type.
 *
 * Returns: Event kind (5000 + job_type)
 */
gint gnostr_dvm_request_kind_for_job(gint job_type);

/**
 * gnostr_dvm_result_kind_for_job:
 * @job_type: Job type (0-999)
 *
 * Gets the result kind for a job type.
 *
 * Returns: Event kind (6000 + job_type)
 */
gint gnostr_dvm_result_kind_for_job(gint job_type);

/**
 * gnostr_dvm_job_type_from_kind:
 * @kind: Event kind (5000-5999 or 6000-6999)
 *
 * Extracts the job type from a request or result kind.
 *
 * Returns: Job type (0-999), or -1 if kind is not a DVM kind
 */
gint gnostr_dvm_job_type_from_kind(gint kind);

/**
 * gnostr_dvm_get_job_type_name:
 * @job_type: Job type (0-999)
 *
 * Gets a human-readable name for common job types.
 *
 * Returns: (transfer none): Static string describing the job type
 */
const gchar *gnostr_dvm_get_job_type_name(gint job_type);

/* ============== Filter Building ============== */

/**
 * gnostr_dvm_build_request_filter:
 * @job_type: Job type to filter for, or -1 for all request types
 * @since: Unix timestamp to filter from, or 0 for no limit
 * @limit: Maximum number of events, or 0 for no limit
 *
 * Builds a NIP-01 filter JSON for querying DVM job requests.
 *
 * Returns: (transfer full): Filter JSON string. Caller must g_free().
 */
gchar *gnostr_dvm_build_request_filter(gint job_type, gint64 since, gint limit);

/**
 * gnostr_dvm_build_result_filter:
 * @request_id: (nullable): Request event ID to find results for
 * @job_type: Job type to filter for, or -1 for all result types
 * @since: Unix timestamp to filter from, or 0 for no limit
 * @limit: Maximum number of events, or 0 for no limit
 *
 * Builds a NIP-01 filter JSON for querying DVM job results.
 *
 * Returns: (transfer full): Filter JSON string. Caller must g_free().
 */
gchar *gnostr_dvm_build_result_filter(const gchar *request_id,
                                       gint job_type,
                                       gint64 since,
                                       gint limit);

/**
 * gnostr_dvm_build_feedback_filter:
 * @request_id: (nullable): Request event ID to find feedback for
 * @since: Unix timestamp to filter from, or 0 for no limit
 * @limit: Maximum number of events, or 0 for no limit
 *
 * Builds a NIP-01 filter JSON for querying DVM job feedback.
 *
 * Returns: (transfer full): Filter JSON string. Caller must g_free().
 */
gchar *gnostr_dvm_build_feedback_filter(const gchar *request_id,
                                         gint64 since,
                                         gint limit);

G_END_DECLS

#endif /* GNOSTR_NIP90_DVM_H */
