#ifndef REALM_SYNC_CLIENT_BASE_HPP
#define REALM_SYNC_CLIENT_BASE_HPP

#include <realm/transaction.hpp>
#include <realm/sync/config.hpp>
#include <realm/sync/protocol.hpp>
#include <realm/sync/socket_provider.hpp>
#include <realm/util/functional.hpp>

namespace realm::sync {
class ClientImpl;
class SessionWrapper;

/// The presence of the ClientReset config indicates an ongoing or requested client
/// reset operation. If client_reset is util::none or if the local Realm does not
/// exist, an ordinary sync session will take place.
///
/// A session will perform client reset by downloading a fresh copy of the Realm
/// from the server at a different file path location. After download, the fresh
/// Realm will be integrated into the local Realm in a write transaction. The
/// application is free to read or write to the local realm during the entire client
/// reset. Like a DOWNLOAD message, the application will not be able to perform a
/// write transaction at the same time as the sync client performs its own write
/// transaction. Client reset is not more disturbing for the application than any
/// DOWNLOAD message. The application can listen to change notifications from the
/// client reset exactly as in a DOWNLOAD message. If the application writes to the
/// local realm during client reset but before the client reset operation has
/// obtained a write lock, the changes made by the application may be lost or
/// overwritten depending on the recovery mode selected.
///
/// Client reset downloads its fresh Realm copy for a Realm at path "xyx.realm" to
/// "xyz.realm.fresh". It is assumed that this path is available for use and if
/// there are any problems the client reset will fail with
/// Client::Error::client_reset_failed.
///
/// The recommended usage of client reset is after a previous session encountered an
/// error that implies the need for a client reset. It is not recommended to persist
/// the need for a client reset. The application should just attempt to synchronize
/// in the usual fashion and only after hitting an error, start a new session with a
/// client reset. In other words, if the application crashes during a client reset,
/// the application should attempt to perform ordinary synchronization after restart
/// and switch to client reset if needed.
///
/// Error codes that imply the need for a client reset are the session level error
/// codes described by SyncError::is_client_reset_requested()
///
/// However, other errors such as bad changeset (UPLOAD) could also be resolved with
/// a client reset. Client reset can even be used without any prior error if so
/// desired.
///
/// After completion of a client reset, the sync client will continue synchronizing
/// with the server in the usual fashion.
///
/// The progress of client reset can be tracked with the standard progress handler.
///
/// Client reset is done when the progress handler arguments satisfy
/// "progress_version > 0". However, if the application wants to ensure that it has
/// all data present on the server, it should wait for download completion using
/// either void async_wait_for_download_completion(WaitOperCompletionHandler) or
/// bool wait_for_download_complete_or_client_stopped().
struct ClientReset {
    realm::ClientResyncMode mode;
    DBRef fresh_copy;
    bool recovery_is_allowed = true;
    util::UniqueFunction<void(VersionID)> notify_before_client_reset;
    util::UniqueFunction<void(VersionID before_version, bool did_recover)> notify_after_client_reset;
};

/// \brief Protocol errors discovered by the client.
///
/// These errors will terminate the network connection (disconnect all sessions
/// associated with the affected connection), and the error will be reported to
/// the application via the connection state change listeners of the affected
/// sessions.
enum class ClientError {
    // clang-format off
    connection_closed           = 100, ///< Connection closed (no error)
    unknown_message             = 101, ///< Unknown type of input message
    bad_syntax                  = 102, ///< Bad syntax in input message head
    limits_exceeded             = 103, ///< Limits exceeded in input message
    bad_session_ident           = 104, ///< Bad session identifier in input message
    bad_message_order           = 105, ///< Bad input message order
    bad_client_file_ident       = 106, ///< Bad client file identifier (IDENT)
    bad_progress                = 107, ///< Bad progress information (DOWNLOAD)
    bad_changeset_header_syntax = 108, ///< Bad syntax in changeset header (DOWNLOAD)
    bad_changeset_size          = 109, ///< Bad changeset size in changeset header (DOWNLOAD)
    bad_origin_file_ident       = 110, ///< Bad origin file identifier in changeset header (DOWNLOAD)
    bad_server_version          = 111, ///< Bad server version in changeset header (DOWNLOAD)
    bad_changeset               = 112, ///< Bad changeset (DOWNLOAD)
    bad_request_ident           = 113, ///< Bad request identifier (MARK)
    bad_error_code              = 114, ///< Bad error code (ERROR),
    bad_compression             = 115, ///< Bad compression (DOWNLOAD)
    bad_client_version          = 116, ///< Bad last integrated client version in changeset header (DOWNLOAD)
    ssl_server_cert_rejected    = 117, ///< SSL server certificate rejected
    pong_timeout                = 118, ///< Timeout on reception of PONG respone message
    bad_client_file_ident_salt  = 119, ///< Bad client file identifier salt (IDENT)
    bad_file_ident              = 120, ///< Bad file identifier (ALLOC)
    connect_timeout             = 121, ///< Sync connection was not fully established in time
    bad_timestamp               = 122, ///< Bad timestamp (PONG)
    bad_protocol_from_server    = 123, ///< Bad or missing protocol version information from server
    client_too_old_for_server   = 124, ///< Protocol version negotiation failed: Client is too old for server
    client_too_new_for_server   = 125, ///< Protocol version negotiation failed: Client is too new for server
    protocol_mismatch           = 126, ///< Protocol version negotiation failed: No version supported by both client and server
    bad_state_message           = 127, ///< Bad values in state message (STATE)
    missing_protocol_feature    = 128, ///< Requested feature missing in negotiated protocol version
    http_tunnel_failed          = 131, ///< Failed to establish HTTP tunnel with configured proxy
    auto_client_reset_failure   = 132, ///< A fatal error was encountered which prevents completion of a client reset
    // clang-format on
};

const std::error_category& client_error_category() noexcept;

std::error_code make_error_code(ClientError) noexcept;

ProtocolError client_error_to_protocol_error(ClientError);
} // namespace realm::sync

namespace std {

template <>
struct is_error_code_enum<realm::sync::ClientError> {
    static const bool value = true;
};

} // namespace std

namespace realm::sync {

static constexpr milliseconds_type default_connect_timeout = 120000;        // 2 minutes
static constexpr milliseconds_type default_connection_linger_time = 30000;  // 30 seconds
static constexpr milliseconds_type default_ping_keepalive_period = 60000;   // 1 minute
static constexpr milliseconds_type default_pong_keepalive_timeout = 120000; // 2 minutes
static constexpr milliseconds_type default_fast_reconnect_limit = 60000;    // 1 minute

using RoundtripTimeHandler = void(milliseconds_type roundtrip_time);

struct ClientConfig {
    /// An optional logger to be used by the client. If no logger is
    /// specified, the client will use an instance of util::StderrLogger
    /// with the log level threshold set to util::Logger::Level::info. The
    /// client does not require a thread-safe logger, and it guarantees that
    /// all logging happens either on behalf of the constructor or on behalf
    /// of the invocation of run().
    std::shared_ptr<util::Logger> logger;

    // The SyncSocket instance used by the Sync Client for event synchronization
    // and creating WebSockets. If not provided the default implementation will be used.
    std::shared_ptr<sync::SyncSocketProvider> socket_provider;

    /// Use ports 80 and 443 by default instead of 7800 and 7801
    /// respectively. Ideally, these default ports should have been made
    /// available via a different URI scheme instead (http/https or ws/wss).
    bool enable_default_port_hack = true;

    /// For testing purposes only.
    ReconnectMode reconnect_mode = ReconnectMode::normal;

    /// Create a separate connection for each session. For testing purposes
    /// only.
    ///
    /// FIXME: This setting needs to be true for now, due to limitations in
    /// the load balancer.
    bool one_connection_per_session = true;

    /// Do not access the local file system. Sessions will act as if
    /// initiated on behalf of an empty (or nonexisting) local Realm
    /// file. Received DOWNLOAD messages will be accepted, but otherwise
    /// ignored. No UPLOAD messages will be generated. For testing purposes
    /// only.
    ///
    /// Many operations, such as serialized transactions, are not suppored
    /// in this mode.
    bool dry_run = false;

    /// The maximum number of milliseconds to allow for a connection to
    /// become fully established. This includes the time to resolve the
    /// network address, the TCP connect operation, the SSL handshake, and
    /// the WebSocket handshake.
    milliseconds_type connect_timeout = default_connect_timeout;

    /// The number of milliseconds to keep a connection open after all
    /// sessions have been abandoned (or suspended by errors).
    ///
    /// The purpose of this linger time is to avoid close/reopen cycles
    /// during short periods of time where there are no sessions interested
    /// in using the connection.
    ///
    /// If the connection gets closed due to an error before the linger time
    /// expires, the connection will be kept closed until there are sessions
    /// willing to use it again.
    milliseconds_type connection_linger_time = default_connection_linger_time;

    /// The client will send PING messages periodically to allow the server
    /// to detect dead connections (heartbeat). This parameter specifies the
    /// time, in milliseconds, between these PING messages. When scheduling
    /// the next PING message, the client will deduct a small random amount
    /// from the specified value to help spread the load on the server from
    /// many clients.
    milliseconds_type ping_keepalive_period = default_ping_keepalive_period;

    /// Whenever the server receives a PING message, it is supposed to
    /// respond with a PONG messsage to allow the client to detect dead
    /// connections (heartbeat). This parameter specifies the time, in
    /// milliseconds, that the client will wait for the PONG response
    /// message before it assumes that the connection is dead, and
    /// terminates it.
    milliseconds_type pong_keepalive_timeout = default_pong_keepalive_timeout;

    /// The maximum amount of time, in milliseconds, since the loss of a
    /// prior connection, for a new connection to be considered a *fast
    /// reconnect*.
    ///
    /// In general, when a client establishes a connection to the server,
    /// the uploading process remains suspended until the initial
    /// downloading process completes (as if by invocation of
    /// Session::async_wait_for_download_completion()). However, to avoid
    /// unnecessary latency in change propagation during ongoing
    /// application-level activity, if the new connection is established
    /// less than a certain amount of time (`fast_reconnect_limit`) since
    /// the client was previously connected to the server, then the
    /// uploading process will be activated immediately.
    ///
    /// For now, the purpose of the general delaying of the activation of
    /// the uploading process, is to increase the chance of multiple initial
    /// transactions on the client-side, to be uploaded to, and processed by
    /// the server as a single unit. In the longer run, the intention is
    /// that the client should upload transformed (from reciprocal history),
    /// rather than original changesets when applicable to reduce the need
    /// for changeset to be transformed on both sides. The delaying of the
    /// upload process will increase the number of cases where this is
    /// possible.
    ///
    /// FIXME: Currently, the time between connections is not tracked across
    /// sessions, so if the application closes its session, and opens a new
    /// one immediately afterwards, the activation of the upload process
    /// will be delayed unconditionally.
    milliseconds_type fast_reconnect_limit = default_fast_reconnect_limit;

    /// Set to true to completely disable delaying of the upload process. In
    /// this mode, the upload process will be activated immediately, and the
    /// value of `fast_reconnect_limit` is ignored.
    ///
    /// For testing purposes only.
    bool disable_upload_activation_delay = false;

    /// If `disable_upload_compaction` is true, every changeset will be
    /// compacted before it is uploaded to the server. Compaction will
    /// reduce the size of a changeset if the same field is set multiple
    /// times or if newly created objects are deleted within the same
    /// transaction. Log compaction increeses CPU usage and memory
    /// consumption.
    bool disable_upload_compaction = false;

    /// The specified function will be called whenever a PONG message is
    /// received on any connection. The round-trip time in milliseconds will
    /// be pased to the function. The specified function will always be
    /// called by the client's event loop thread, i.e., the thread that
    /// calls `Client::run()`. This feature is mainly for testing purposes.
    std::function<RoundtripTimeHandler> roundtrip_time_handler;

    /// Disable sync to disk (fsync(), msync()) for all realm files managed
    /// by this client.
    ///
    /// Testing/debugging feature. Should never be enabled in production.
    bool disable_sync_to_disk = false;

    /// The sync client supports tables without primary keys by synthesizing a
    /// pk using the client file ident, which means that all changesets waiting
    /// to be uploaded need to be rewritten with the correct ident the first time
    /// we connect to the server. The modern server doesn't support this and
    /// requires pks for all tables, so this is now only applicable to old sync
    /// tests and so is disabled by default.
    bool fix_up_object_ids = false;
};

/// \brief Information about an error causing a session to be temporarily
/// disconnected from the server.
///
/// In general, the connection will be automatically reestablished
/// later. Whether this happens quickly, generally depends on \ref
/// is_fatal. If \ref is_fatal is true, it means that the error is deemed to
/// be of a kind that is likely to persist, and cause all future reconnect
/// attempts to fail. In that case, if another attempt is made at
/// reconnecting, the delay will be substantial (at least an hour).
///
/// \ref error_code specifies the error that caused the connection to be
/// closed. For the list of errors reported by the server, see \ref
/// ProtocolError (or `protocol.md`). For the list of errors corresponding
/// to protocol violations that are detected by the client, see
/// Client::Error. The error may also be a system level error, or an error
/// from one of the potential intermediate protocol layers (SSL or
/// WebSocket).
///
/// \ref detailed_message is the most detailed message available to describe
/// the error. It is generally equal to `error_code.message()`, but may also
/// be a more specific message (one that provides extra context). The
/// purpose of this message is mostly to aid in debugging. For non-debugging
/// purposes, `error_code.message()` should generally be considered
/// sufficient.
///
/// \sa set_connection_state_change_listener().
struct SessionErrorInfo : public ProtocolErrorInfo {
    SessionErrorInfo(const ProtocolErrorInfo& info, const std::error_code& ec)
        : ProtocolErrorInfo(info)
        , error_code(ec)
    {
    }
    SessionErrorInfo(const std::error_code& ec, bool try_again)
        : ProtocolErrorInfo(ec.value(), ec.message(), try_again)
        , error_code(ec)
    {
    }
    SessionErrorInfo(const std::error_code& ec, const std::string& msg, bool try_again)
        : ProtocolErrorInfo(ec.value(), msg, try_again)
        , error_code(ec)
    {
    }
    std::error_code error_code;
};

enum class ConnectionState { disconnected, connecting, connected };

} // namespace realm::sync

#endif // REALM_SYNC_CLIENT_BASE_HPP
