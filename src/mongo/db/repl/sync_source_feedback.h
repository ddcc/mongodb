/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/client/constants.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
class OperationContext;

namespace repl {

class SyncSourceFeedback {
public:
    /// Notifies the SyncSourceFeedbackThread to wake up and send an update upstream of slave
    /// replication progress.
    void forwardSlaveProgress();

    /**
     * Loops continuously until shutdown() is called, passing updates when they are present. If no
     * update occurs within the _keepAliveInterval, progress is forwarded to let the upstream node
     * know that this node, along with the alive nodes chaining through it, are still alive.
     */
    void run(ReplicationCoordinator* replCoord);

    /// Signals the run() method to terminate.
    void shutdown();

private:
    void _resetConnection();

    /**
     * Authenticates _connection using the server's cluster-membership credentials.
     *
     * Returns true on successful authentication.
     */
    bool replAuthenticate();

    /* Inform the sync target of our current position in the oplog, as well as the positions
     * of all secondaries chained through us.
     * "oldStyle" indicates whether or not the upstream node is pre-3.2.2 and needs the older style
     * ReplSetUpdatePosition commands as a result.
     */
    Status updateUpstream(ReplicationCoordinator* replCoord, bool oldStyle);

    bool hasConnection() {
        return _connection.get();
    }

    /// Connect to sync target.
    bool _connect(const ReplicaSetConfig& rsConfig, const HostAndPort& host);

    // the member we are currently syncing from
    HostAndPort _syncTarget;
    // our connection to our sync target
    std::unique_ptr<DBClientConnection> _connection;
    // protects cond, _shutdownSignaled, _keepAliveInterval, and _positionChanged.
    stdx::mutex _mtx;
    // used to alert our thread of changes which need to be passed up the chain
    stdx::condition_variable _cond;
    /// _keepAliveInterval indicates how frequently to forward progress in the absence of updates.
    Milliseconds _keepAliveInterval = Milliseconds(100);
    // used to indicate a position change which has not yet been pushed along
    bool _positionChanged = false;
    // Once this is set to true the _run method will terminate
    bool _shutdownSignaled = false;
    // Indicates whether our syncSource can't accept the new version of the UpdatePosition command.
    bool _fallBackToOldUpdatePosition = false;
};
}  // namespace repl
}  // namespace mongo
