```c
/* xts_api.h
 *
 * XTS (X Time Series) External C API (Stable ABI)
 *
 * Design goals:
 * - Pure C (C99), language-agnostic FFI
 * - Opaque handles, no C++ ABI exposure
 * - Error-code based (int32), thread-safe error text retrieval
 * - Caller/Library memory ownership explicitly defined
 *
 * Notes on memory ownership:
 * - Any function returning heap memory via out pointer (e.g., list tags / read history)
 *   must be released by the corresponding xts_*_free_* function.
 * - Strings returned inside returned arrays (e.g., xts_tag_info_t.name/unit/desc)
 *   are owned by the library and remain valid until the array is freed.
 * - For "single object output" structures containing const char* (e.g., xts_meta_get_tag),
 *   those const char* are owned by the library and remain valid until:
 *     - the next call to the same API on the same connection, or
 *     - the connection is closed,
 *   unless otherwise stated by implementation. Prefer xts_meta_list_tags if you need stable lifetime.
 */

#ifndef XTS_API_H
#define XTS_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* =========================
 *  Platform / Visibility
 * ========================= */

#include <stdint.h>
#include <stddef.h>

#if !defined(XTS_CALL)
#  if defined(_WIN32) || defined(__CYGWIN__)
#    define XTS_CALL __cdecl
#  else
#    define XTS_CALL
#  endif
#endif

#if !defined(XTS_API)
#  if defined(_WIN32) || defined(__CYGWIN__)
#    if defined(XTS_BUILD_DLL)
#      define XTS_API __declspec(dllexport)
#    elif defined(XTS_USE_DLL)
#      define XTS_API __declspec(dllimport)
#    else
#      define XTS_API
#    endif
#  else
#    if defined(__GNUC__) && __GNUC__ >= 4
#      define XTS_API __attribute__((visibility("default")))
#    else
#      define XTS_API
#    endif
#  endif
#endif

#if !defined(XTS_NULL)
#  define XTS_NULL ((void*)0)
#endif

/* =========================
 *  ABI Versioning
 * ========================= */

/* Semantic: MAJOR breaks ABI, MINOR adds backwards-compatible APIs/fields.
 * PATCH is for implementation only (not represented here).
 */
#define XTS_API_VERSION_MAJOR  1
#define XTS_API_VERSION_MINOR  0

/* Returns packed version: (major << 16) | minor */
XTS_API uint32_t XTS_CALL xts_api_version(void);

/* =========================
 *  Error Codes
 * ========================= */

typedef int32_t xts_err_t;

enum {
    XTS_OK                 = 0,

    XTS_ERR_INVALID_ARG    = -1,
    XTS_ERR_NOT_FOUND      = -2,
    XTS_ERR_TIMEOUT        = -3,
    XTS_ERR_NO_MEMORY      = -4,
    XTS_ERR_IO             = -5,
    XTS_ERR_PERMISSION     = -6,
    XTS_ERR_NOT_SUPPORTED  = -7,
    XTS_ERR_BUSY           = -8,
    XTS_ERR_CLOSED         = -9,

    XTS_ERR_INTERNAL       = -100
};

/* =========================
 *  Core Types
 * ========================= */

typedef void* xts_handle_t;

/* milliseconds since Unix epoch (1970-01-01T00:00:00Z) */
typedef int64_t xts_timestamp_t;

/* Quality: compatible with common industrial conventions */
typedef enum xts_quality_t {
    XTS_QUALITY_INVALID   = 0,
    XTS_QUALITY_GOOD      = 1,
    XTS_QUALITY_BAD       = 2,
    XTS_QUALITY_UNCERTAIN = 3,
    XTS_QUALITY_LIMIT     = 4
} xts_quality_t;

/* Variant value type */
typedef enum xts_value_type_t {
    XTS_TYPE_NULL   = 0,
    XTS_TYPE_INT32  = 1,
    XTS_TYPE_INT64  = 2,
    XTS_TYPE_FLOAT  = 3,
    XTS_TYPE_DOUBLE = 4,
    XTS_TYPE_BOOL   = 5,
    XTS_TYPE_STRING = 6,
    XTS_TYPE_BYTES  = 7 /* optional: opaque binary */
} xts_value_type_t;

typedef struct xts_bytes_t {
    const uint8_t* data;   /* owned by library unless otherwise documented */
    int32_t        size;   /* bytes */
} xts_bytes_t;

typedef struct xts_value_t {
    xts_value_type_t type;
    union {
        int32_t     i32;
        int64_t     i64;
        float       f32;
        double      f64;
        int32_t     b;     /* 0 or 1 */
        const char* s;     /* UTF-8, NUL-terminated */
        xts_bytes_t bytes; /* for XTS_TYPE_BYTES */
    } v;
} xts_value_t;

/* A single time-series point */
typedef struct xts_point_t {
    xts_timestamp_t ts;
    xts_value_t     value;
    xts_quality_t   quality;
} xts_point_t;

/* =========================
 *  Connection / Session
 * ========================= */

/* endpoint examples:
 *   "xts://127.0.0.1:9000"
 *   "xts+tls://db.company.com:9443"
 *
 * user/password may be NULL for anonymous or token-based modes.
 */
XTS_API xts_err_t XTS_CALL xts_conn_open(
    const char*   endpoint,
    const char*   user,
    const char*   password,
    xts_handle_t* out_conn
);

XTS_API xts_err_t XTS_CALL xts_conn_close(
    xts_handle_t conn
);

/* Generic option setter (implementation-defined).
 * Examples:
 *   key="timeout_ms", value="5000"
 *   key="tls.ca_file", value="/path/ca.pem"
 *   key="compress", value="lz4"
 */
XTS_API xts_err_t XTS_CALL xts_conn_set_option(
    xts_handle_t conn,
    const char*  key,
    const char*  value
);

/* Ping / health check (optional but useful for SDK users) */
XTS_API xts_err_t XTS_CALL xts_conn_ping(
    xts_handle_t conn
);

/* =========================
 *  Error Message (Thread-safe)
 * ========================= */

/* Retrieve last error message for a connection (or global if conn==NULL).
 * - buffer may be NULL to query required length (including NUL).
 * - out_required_len may be NULL.
 */
XTS_API xts_err_t XTS_CALL xts_err_last_message(
    xts_handle_t conn,
    char*        buffer,
    int32_t      buffer_len,
    int32_t*     out_required_len
);

/* =========================
 *  Metadata / Tag Catalog
 * ========================= */

typedef struct xts_tag_info_t {
    const char* name;        /* fully-qualified tag name (UTF-8) */
    const char* unit;        /* engineering unit (UTF-8), may be NULL */
    const char* desc;        /* description (UTF-8), may be NULL */
    int32_t     writable;    /* 0/1 */
    int32_t     reserved0;   /* keep for future ABI extension */
} xts_tag_info_t;

/* List tags by pattern.
 * pattern examples (implementation-defined):
 *   "*" or "plant1.*" or "area/line/*"
 * If pattern==NULL, list all tags (may be expensive).
 *
 * Returns:
 *   out_tags points to an array of xts_tag_info_t of length out_count.
 *   Release with xts_meta_free_tags.
 */
XTS_API xts_err_t XTS_CALL xts_meta_list_tags(
    xts_handle_t      conn,
    const char*       pattern,
    xts_tag_info_t**  out_tags,
    int32_t*          out_count
);

XTS_API xts_err_t XTS_CALL xts_meta_free_tags(
    xts_tag_info_t* tags,
    int32_t         count
);

/* Get one tag info (strings owned by library; see header notes). */
XTS_API xts_err_t XTS_CALL xts_meta_get_tag(
    xts_handle_t     conn,
    const char*      tag_name,
    xts_tag_info_t*  out_info
);

/* Optional: resolve tag name to internal ID for faster operations */
typedef int64_t xts_tag_id_t; /* opaque numeric id */

XTS_API xts_err_t XTS_CALL xts_meta_resolve_tag_id(
    xts_handle_t   conn,
    const char*    tag_name,
    xts_tag_id_t*  out_tag_id
);

/* =========================
 *  Real-time Read/Write (Snapshot)
 * ========================= */

XTS_API xts_err_t XTS_CALL xts_rt_read(
    xts_handle_t      conn,
    const char*       tag_name,
    xts_value_t*      out_value,
    xts_quality_t*    out_quality,
    xts_timestamp_t*  out_ts
);

XTS_API xts_err_t XTS_CALL xts_rt_write(
    xts_handle_t       conn,
    const char*        tag_name,
    const xts_value_t* value
);

/* Batch read:
 * - tag_names is an array of const char* (length tag_count)
 * - out_values/out_qualities/out_timestamps are caller-allocated arrays of length tag_count
 * - function returns XTS_OK even if some tags fail; per-tag status can be queried via out_statuses if provided
 */
XTS_API xts_err_t XTS_CALL xts_rt_read_batch(
    xts_handle_t       conn,
    const char**       tag_names,
    int32_t            tag_count,
    xts_value_t*       out_values,
    xts_quality_t*     out_qualities,
    xts_timestamp_t*   out_timestamps,
    xts_err_t*         out_statuses /* optional, may be NULL */
);

/* Optional: ID-based read/write (faster; avoids repeated name lookup) */
XTS_API xts_err_t XTS_CALL xts_rt_read_by_id(
    xts_handle_t      conn,
    xts_tag_id_t      tag_id,
    xts_value_t*      out_value,
    xts_quality_t*    out_quality,
    xts_timestamp_t*  out_ts
);

XTS_API xts_err_t XTS_CALL xts_rt_write_by_id(
    xts_handle_t       conn,
    xts_tag_id_t       tag_id,
    const xts_value_t* value
);

/* =========================
 *  Historical Queries
 * ========================= */

/* Read raw points in [start, end).
 * - Returned points must be freed by xts_hist_free_points().
 */
XTS_API xts_err_t XTS_CALL xts_hist_read_raw(
    xts_handle_t      conn,
    const char*       tag_name,
    xts_timestamp_t   start,
    xts_timestamp_t   end,
    xts_point_t**     out_points,
    int32_t*          out_count
);

XTS_API xts_err_t XTS_CALL xts_hist_free_points(
    void* points /* xts_point_t* or other hist allocations */
);

/* Aggregations */
typedef enum xts_agg_type_t {
    XTS_AGG_AVG    = 1,
    XTS_AGG_MIN    = 2,
    XTS_AGG_MAX    = 3,
    XTS_AGG_SUM    = 4,
    XTS_AGG_COUNT  = 5,
    XTS_AGG_STDDEV = 6
} xts_agg_type_t;

typedef struct xts_agg_point_t {
    xts_timestamp_t start;
    xts_timestamp_t end;
    xts_value_t     value;
    xts_quality_t   quality;
} xts_agg_point_t;

/* Read aggregation over fixed windows.
 * - interval_ms must be > 0
 * - Returned array must be freed via xts_hist_free_points()
 */
XTS_API xts_err_t XTS_CALL xts_hist_read_agg(
    xts_handle_t       conn,
    const char*        tag_name,
    xts_timestamp_t    start,
    xts_timestamp_t    end,
    int64_t            interval_ms,
    xts_agg_type_t     agg,
    xts_agg_point_t**  out_points,
    int32_t*           out_count
);

/* Optional: "at time" interpolation (PHD/PI style):
 *   - mode could be "previous", "next", "linear"
 */
typedef enum xts_interp_t {
    XTS_INTERP_PREVIOUS = 1,
    XTS_INTERP_NEXT     = 2,
    XTS_INTERP_LINEAR   = 3
} xts_interp_t;

XTS_API xts_err_t XTS_CALL xts_hist_read_at(
    xts_handle_t      conn,
    const char*       tag_name,
    xts_timestamp_t   ts,
    xts_interp_t      mode,
    xts_value_t*      out_value,
    xts_quality_t*    out_quality,
    xts_timestamp_t*  out_actual_ts /* optional, may be NULL */
);

/* =========================
 *  Subscription / Streaming
 * ========================= */

/* Subscription callback:
 * - tag_name is UTF-8, owned by library for duration of callback
 * - value pointer valid only during callback
 */
typedef void (*xts_sub_callback_t)(
    const char*         tag_name,
    const xts_value_t*  value,
    xts_quality_t       quality,
    xts_timestamp_t     ts,
    void*               user_data
);

/* Create subscription:
 * - interval_ms: suggested sampling / push interval; 0 means "on change" if supported
 * - out_sub is a handle; destroy with xts_unsubscribe
 */
XTS_API xts_err_t XTS_CALL xts_subscribe(
    xts_handle_t        conn,
    const char**        tag_names,
    int32_t             tag_count,
    int32_t             interval_ms,
    xts_sub_callback_t  cb,
    void*               user_data,
    xts_handle_t*       out_sub
);

XTS_API xts_err_t XTS_CALL xts_unsubscribe(
    xts_handle_t sub
);

/* Optional: drive callback dispatch from user's thread (poll model).
 * If implementation uses internal threads, these may be no-ops.
 */
XTS_API xts_err_t XTS_CALL xts_sub_poll(
    xts_handle_t sub,
    int32_t      timeout_ms
);

/* =========================
 *  Utilities
 * ========================= */

/* Initialize a value as NULL */
XTS_API void XTS_CALL xts_value_set_null(xts_value_t* v);

/* Deep copy: copies string/bytes into library-owned memory.
 * - out_value becomes library-owned; caller must later call xts_value_free() if it contains owned memory.
 * - This is optional but extremely helpful for FFI languages to avoid lifetime bugs.
 */
XTS_API xts_err_t XTS_CALL xts_value_deep_copy(
    const xts_value_t* in_value,
    xts_value_t*       out_value
);

/* Free resources held by a value created by xts_value_deep_copy or returned by some APIs (implementation-defined). */
XTS_API void XTS_CALL xts_value_free(xts_value_t* v);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XTS_API_H */
```
