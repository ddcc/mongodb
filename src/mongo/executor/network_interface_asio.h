/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <asio.hpp>

#include <boost/optional.hpp>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/system_error.h"
#include "mongo/executor/async_stream_factory_interface.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/async_timer_interface.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/rpc/protocol.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/net/message.h"

namespace mongo {

namespace executor {

namespace connection_pool_asio {
class ASIOConnection;
class ASIOTimer;
class ASIOImpl;
}  // connection_pool_asio

class AsyncStreamInterface;

/**
 * Implementation of the replication system's network interface using Christopher
 * Kohlhoff's ASIO library instead of existing MongoDB networking primitives.
 */
class NetworkInterfaceASIO final : public NetworkInterface {
    friend class connection_pool_asio::ASIOConnection;
    friend class connection_pool_asio::ASIOTimer;
    friend class connection_pool_asio::ASIOImpl;

public:
    struct Options {
        Options();

// Explicit move construction and assignment to support MSVC
#if defined(_MSC_VER) && _MSC_VER < 1900
        Options(Options&&);
        Options& operator=(Options&&);
#else
        Options(Options&&) = default;
        Options& operator=(Options&&) = default;
#endif

        std::string instanceName = "NetworkInterfaceASIO";
        ConnectionPool::Options connectionPoolOptions;
        std::unique_ptr<AsyncTimerFactoryInterface> timerFactory;
        std::unique_ptr<NetworkConnectionHook> networkConnectionHook;
        std::unique_ptr<AsyncStreamFactoryInterface> streamFactory;
        std::unique_ptr<rpc::EgressMetadataHook> metadataHook;
    };

    NetworkInterfaceASIO(Options = Options());

    std::string getDiagnosticString() override;
    void appendConnectionStats(ConnectionPoolStats* stats) const override;
    std::string getHostName() override;
    void startup() override;
    void shutdown() override;
    void waitForWork() override;
    void waitForWorkUntil(Date_t when) override;
    void signalWorkAvailable() override;
    Date_t now() override;
    void startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                      const RemoteCommandRequest& request,
                      const RemoteCommandCompletionFn& onFinish) override;
    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) override;
    void cancelAllCommands() override;
    void setAlarm(Date_t when, const stdx::function<void()>& action) override;

    bool onNetworkThread() override;

    bool inShutdown() const;

    void dropConnections(const HostAndPort& hostAndPort) override;

private:
    using ResponseStatus = TaskExecutor::ResponseStatus;
    using NetworkInterface::RemoteCommandCompletionFn;
    using NetworkOpHandler = stdx::function<void(std::error_code, size_t)>;

    enum class State { kReady, kRunning, kShutdown };

    friend class AsyncOp;

    /**
     * AsyncConnection encapsulates the per-connection state we maintain.
     */
    class AsyncConnection {
    public:
        AsyncConnection(std::unique_ptr<AsyncStreamInterface>, rpc::ProtocolSet serverProtocols);

        AsyncStreamInterface& stream();

        void cancel();

        rpc::ProtocolSet serverProtocols() const;
        rpc::ProtocolSet clientProtocols() const;
        void setServerProtocols(rpc::ProtocolSet protocols);

// Explicit move construction and assignment to support MSVC
#if defined(_MSC_VER) && _MSC_VER < 1900
        AsyncConnection(AsyncConnection&&);
        AsyncConnection& operator=(AsyncConnection&&);
#else
        AsyncConnection(AsyncConnection&&) = default;
        AsyncConnection& operator=(AsyncConnection&&) = default;
#endif

    private:
        std::unique_ptr<AsyncStreamInterface> _stream;

        rpc::ProtocolSet _serverProtocols;

        // Dynamically initialized from [min max]WireVersionOutgoing.
        // Its expected that isMaster response is checked only on the caller.
        rpc::ProtocolSet _clientProtocols{rpc::supports::kNone};
    };

    /**
     * AsyncCommand holds state for a currently running or soon-to-be-run command.
     */
    class AsyncCommand {
    public:
        /**
         * Describes the variant of AsyncCommand this object represents.
         */
        enum class CommandType {
            /**
             * An ordinary command of an unspecified Protocol.
             */
            kRPC,

            /**
             * A 'find' command that has been downconverted to an OP_QUERY.
             */
            kDownConvertedFind,

            /**
             * A 'getMore' command that has been downconverted to an OP_GET_MORE.
             */
            kDownConvertedGetMore,
        };

        AsyncCommand(AsyncConnection* conn,
                     CommandType type,
                     Message&& command,
                     Date_t now,
                     const HostAndPort& target);

        NetworkInterfaceASIO::AsyncConnection& conn();

        Message& toSend();
        Message& toRecv();
        MSGHEADER::Value& header();

        ResponseStatus response(rpc::Protocol protocol,
                                Date_t now,
                                rpc::EgressMetadataHook* metadataHook = nullptr);

    private:
        NetworkInterfaceASIO::AsyncConnection* const _conn;

        const CommandType _type;

        Message _toSend;
        Message _toRecv;

        // TODO: Investigate efficiency of storing header separately.
        MSGHEADER::Value _header;

        const Date_t _start;

        HostAndPort _target;
    };

    /**
     * Helper object to manage individual network operations.
     */
    class AsyncOp {
        friend class NetworkInterfaceASIO;
        friend class connection_pool_asio::ASIOConnection;

    public:
        AsyncOp(NetworkInterfaceASIO* net,
                const TaskExecutor::CallbackHandle& cbHandle,
                const RemoteCommandRequest& request,
                const RemoteCommandCompletionFn& onFinish,
                Date_t now);

        /**
         * Access control for AsyncOp. These objects should be used through shared_ptrs.
         *
         * In order to safely access an AsyncOp:
         * 1. Take the lock
         * 2. Check the id
         * 3. If id matches saved generation, proceed, otherwise op has been recycled.
         */
        struct AccessControl {
            stdx::mutex mutex;
            std::size_t id = 0;
        };

        void cancel();
        bool canceled() const;
        bool timedOut() const;

        const TaskExecutor::CallbackHandle& cbHandle() const;

        AsyncConnection& connection();

        void setConnection(AsyncConnection&& conn);

        // AsyncOp may run multiple commands over its lifetime (for example, an ismaster
        // command, the command provided to the NetworkInterface via startCommand(), etc.)
        // Calling beginCommand() resets internal state to prepare to run newCommand.
        Status beginCommand(const RemoteCommandRequest& request,
                            rpc::EgressMetadataHook* metadataHook = nullptr);

        // This form of beginCommand takes a raw message. It is needed if the caller
        // has to form the command manually (e.g. to use a specific requestBuilder).
        Status beginCommand(Message&& newCommand,
                            AsyncCommand::CommandType,
                            const HostAndPort& target);

        AsyncCommand* command();

        void finish(const TaskExecutor::ResponseStatus& status);

        const RemoteCommandRequest& request() const;

        Date_t start() const;

        rpc::Protocol operationProtocol() const;

        void setOperationProtocol(rpc::Protocol proto);

        void reset();

        void setOnFinish(RemoteCommandCompletionFn&& onFinish);

        asio::io_service::strand& strand() {
            return _strand;
        }

        asio::ip::tcp::resolver& resolver() {
            return _resolver;
        }

    private:
        NetworkInterfaceASIO* const _owner;
        // Information describing a task enqueued on the NetworkInterface
        // via a call to startCommand().
        TaskExecutor::CallbackHandle _cbHandle;
        RemoteCommandRequest _request;
        RemoteCommandCompletionFn _onFinish;

        // AsyncOp's have a handle to their connection pool handle. They are
        // also owned by it when they're in the pool
        ConnectionPool::ConnectionHandle _connectionPoolHandle;

        /**
         * The connection state used to service this request. We wrap it in an optional
         * as it is instantiated at some point after the AsyncOp is created.
         */
        boost::optional<AsyncConnection> _connection;

        /**
         * The RPC protocol used for this operation. We wrap it in an optional as it
         * is not known until we obtain a connection.
         */
        boost::optional<rpc::Protocol> _operationProtocol;

        Date_t _start;
        std::unique_ptr<AsyncTimerInterface> _timeoutAlarm;

        asio::ip::tcp::resolver _resolver;

        bool _canceled = false;
        bool _timedOut = false;

        /**
         * We maintain a shared_ptr to an access control object. This ensures that tangent
         * execution paths, such as timeouts for this operation, will not try to access its
         * state after it has been cleaned up.
         */
        std::shared_ptr<AccessControl> _access;

        /**
         * An AsyncOp may run 0, 1, or multiple commands over its lifetime.
         * AsyncOp only holds at most a single AsyncCommand object at a time,
         * representing its current running or next-to-be-run command, if there is one.
         */
        boost::optional<AsyncCommand> _command;
        bool _inSetup;
        bool _inRefresh;

        /**
         * The explicit strand that all operations for this op must run on.
         * This must be the last member of AsyncOp because any pending
         * operation for the strand are run when it's dtor is called. Any
         * members that fall after it will have already been destroyed, which
         * will make those fields illegal to touch from callbacks.
         */
        asio::io_service::strand _strand;
    };

    void _startCommand(AsyncOp* op);

    /**
     * Wraps a completion handler in pre-condition checks.
     * When we resume after an asynchronous call, we may find the following:
     *    - the AsyncOp has been canceled in the interim (via cancelCommand())
     *    - the asynchronous call has returned a non-OK error code
     * Should both conditions be present, we handle cancelation over errors. States use
     * _validateAndRun() to perform these checks before advancing the state machine.
     */
    template <typename Handler>
    void _validateAndRun(AsyncOp* op, std::error_code ec, Handler&& handler) {
        if (op->canceled())
            return _completeOperation(op,
                                      Status(ErrorCodes::CallbackCanceled, "Callback canceled"));
        if (op->timedOut())
            return _completeOperation(op,
                                      Status(ErrorCodes::ExceededTimeLimit, "Operation timed out"));
        if (ec)
            return _networkErrorCallback(op, ec);

        handler();
    }

    // Connection
    void _connect(AsyncOp* op);

    // setup plaintext TCP socket
    void _setupSocket(AsyncOp* op, asio::ip::tcp::resolver::iterator endpoints);

    void _runIsMaster(AsyncOp* op);
    void _runConnectionHook(AsyncOp* op);
    void _authenticate(AsyncOp* op);

    // Communication state machine
    void _beginCommunication(AsyncOp* op);
    void _completedOpCallback(AsyncOp* op);
    void _networkErrorCallback(AsyncOp* op, const std::error_code& ec);
    void _completeOperation(AsyncOp* op, const TaskExecutor::ResponseStatus& resp);

    void _signalWorkAvailable_inlock();

    void _asyncRunCommand(AsyncOp* op, NetworkOpHandler handler);

    Options _options;

    asio::io_service _io_service;
    std::vector<stdx::thread> _serviceRunners;

    const std::unique_ptr<rpc::EgressMetadataHook> _metadataHook;

    const std::unique_ptr<NetworkConnectionHook> _hook;

    std::atomic<State> _state;  // NOLINT

    std::unique_ptr<AsyncTimerFactoryInterface> _timerFactory;

    std::unique_ptr<AsyncStreamFactoryInterface> _streamFactory;

    ConnectionPool _connectionPool;

    // If it is necessary to hold this lock while accessing a particular operation with
    // an AccessControl object, take this lock first, always.
    stdx::mutex _inProgressMutex;
    std::unordered_map<AsyncOp*, std::unique_ptr<AsyncOp>> _inProgress;
    std::unordered_set<TaskExecutor::CallbackHandle> _inGetConnection;

    stdx::mutex _executorMutex;
    bool _isExecutorRunnable;
    stdx::condition_variable _isExecutorRunnableCondition;

    /**
     * The explicit strand that all non-op operations run on. This must be the
     * last member of NetworkInterfaceASIO because any pending operation for
     * the strand are run when it's dtor is called. Any members that fall after
     * it will have already been destroyed, which will make those fields
     * illegal to touch from callbacks.
     */
    asio::io_service::strand _strand;
};

template <typename T, typename R, typename... MethodArgs, typename... DeducedArgs>
R callNoexcept(T& obj, R (T::*method)(MethodArgs...), DeducedArgs&&... args) {
    try {
        return (obj.*method)(std::forward<DeducedArgs>(args)...);
    } catch (...) {
        std::terminate();
    }
}

}  // namespace executor
}  // namespace mongo
