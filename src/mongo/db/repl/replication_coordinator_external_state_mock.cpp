/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_external_state_mock.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/operation_context_repl_mock.h"
#include "mongo/db/storage/snapshot_name.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/sequence_util.h"

namespace mongo {
namespace repl {

ReplicationCoordinatorExternalStateMock::ReplicationCoordinatorExternalStateMock()
    : _localRsConfigDocument(ErrorCodes::NoMatchingDocument, "No local config document"),
      _localRsLastVoteDocument(ErrorCodes::NoMatchingDocument, "No local lastVote document"),
      _lastOpTime(ErrorCodes::NoMatchingDocument, "No last oplog entry"),
      _canAcquireGlobalSharedLock(true),
      _storeLocalConfigDocumentStatus(Status::OK()),
      _storeLocalLastVoteDocumentStatus(Status::OK()),
      _storeLocalConfigDocumentShouldHang(false),
      _storeLocalLastVoteDocumentShouldHang(false),
      _isApplierSignaledToCancelFetcher(false),
      _connectionsClosed(false),
      _threadsStarted(false) {}

ReplicationCoordinatorExternalStateMock::~ReplicationCoordinatorExternalStateMock() {}

void ReplicationCoordinatorExternalStateMock::startThreads(const ReplSettings& settings) {
    _threadsStarted = true;
}

void ReplicationCoordinatorExternalStateMock::startMasterSlave(OperationContext*) {}
Status ReplicationCoordinatorExternalStateMock::initializeReplSetStorage(OperationContext* txn,
                                                                         const BSONObj& config,
                                                                         bool updateReplOpTime) {
    return storeLocalConfigDocument(txn, config);
}

void ReplicationCoordinatorExternalStateMock::shutdown(OperationContext*) {}
void ReplicationCoordinatorExternalStateMock::forwardSlaveProgress() {}

OID ReplicationCoordinatorExternalStateMock::ensureMe(OperationContext*) {
    return OID::gen();
}

bool ReplicationCoordinatorExternalStateMock::isSelf(const HostAndPort& host) {
    return sequenceContains(_selfHosts, host);
}

void ReplicationCoordinatorExternalStateMock::addSelf(const HostAndPort& host) {
    _selfHosts.push_back(host);
}

HostAndPort ReplicationCoordinatorExternalStateMock::getClientHostAndPort(
    const OperationContext* txn) {
    return _clientHostAndPort;
}

void ReplicationCoordinatorExternalStateMock::setClientHostAndPort(
    const HostAndPort& clientHostAndPort) {
    _clientHostAndPort = clientHostAndPort;
}

StatusWith<BSONObj> ReplicationCoordinatorExternalStateMock::loadLocalConfigDocument(
    OperationContext* txn) {
    return _localRsConfigDocument;
}

Status ReplicationCoordinatorExternalStateMock::storeLocalConfigDocument(OperationContext* txn,
                                                                         const BSONObj& config) {
    {
        stdx::unique_lock<stdx::mutex> lock(_shouldHangConfigMutex);
        while (_storeLocalConfigDocumentShouldHang) {
            _shouldHangConfigCondVar.wait(lock);
        }
    }
    if (_storeLocalConfigDocumentStatus.isOK()) {
        setLocalConfigDocument(StatusWith<BSONObj>(config));
        return Status::OK();
    }
    return _storeLocalConfigDocumentStatus;
}

void ReplicationCoordinatorExternalStateMock::setLocalConfigDocument(
    const StatusWith<BSONObj>& localConfigDocument) {
    _localRsConfigDocument = localConfigDocument;
}

StatusWith<LastVote> ReplicationCoordinatorExternalStateMock::loadLocalLastVoteDocument(
    OperationContext* txn) {
    return _localRsLastVoteDocument;
}

Status ReplicationCoordinatorExternalStateMock::storeLocalLastVoteDocument(
    OperationContext* txn, const LastVote& lastVote) {
    {
        stdx::unique_lock<stdx::mutex> lock(_shouldHangLastVoteMutex);
        while (_storeLocalLastVoteDocumentShouldHang) {
            _shouldHangLastVoteCondVar.wait(lock);
        }
    }
    if (_storeLocalLastVoteDocumentStatus.isOK()) {
        setLocalLastVoteDocument(StatusWith<LastVote>(lastVote));
        return Status::OK();
    }
    return _storeLocalLastVoteDocumentStatus;
}

void ReplicationCoordinatorExternalStateMock::setLocalLastVoteDocument(
    const StatusWith<LastVote>& localLastVoteDocument) {
    _localRsLastVoteDocument = localLastVoteDocument;
}

void ReplicationCoordinatorExternalStateMock::setGlobalTimestamp(const Timestamp& newTime) {}

void ReplicationCoordinatorExternalStateMock::cleanUpLastApplyBatch(OperationContext* txn) {}

StatusWith<OpTime> ReplicationCoordinatorExternalStateMock::loadLastOpTime(OperationContext* txn) {
    return _lastOpTime;
}

void ReplicationCoordinatorExternalStateMock::setLastOpTime(const StatusWith<OpTime>& lastApplied) {
    _lastOpTime = lastApplied;
}

void ReplicationCoordinatorExternalStateMock::setStoreLocalConfigDocumentStatus(Status status) {
    _storeLocalConfigDocumentStatus = status;
}

void ReplicationCoordinatorExternalStateMock::setStoreLocalConfigDocumentToHang(bool hang) {
    stdx::unique_lock<stdx::mutex> lock(_shouldHangConfigMutex);
    _storeLocalConfigDocumentShouldHang = hang;
    if (!hang) {
        _shouldHangConfigCondVar.notify_all();
    }
}

bool ReplicationCoordinatorExternalStateMock::isApplierSignaledToCancelFetcher() const {
    return _isApplierSignaledToCancelFetcher;
}

bool ReplicationCoordinatorExternalStateMock::threadsStarted() const {
    return _threadsStarted;
}

void ReplicationCoordinatorExternalStateMock::setStoreLocalLastVoteDocumentStatus(Status status) {
    _storeLocalLastVoteDocumentStatus = status;
}

void ReplicationCoordinatorExternalStateMock::setStoreLocalLastVoteDocumentToHang(bool hang) {
    stdx::unique_lock<stdx::mutex> lock(_shouldHangLastVoteMutex);
    _storeLocalLastVoteDocumentShouldHang = hang;
    if (!hang) {
        _shouldHangLastVoteCondVar.notify_all();
    }
}

void ReplicationCoordinatorExternalStateMock::closeConnections() {
    _connectionsClosed = true;
}

void ReplicationCoordinatorExternalStateMock::killAllUserOperations(OperationContext* txn) {}

void ReplicationCoordinatorExternalStateMock::clearShardingState() {}

void ReplicationCoordinatorExternalStateMock::recoverShardingState(OperationContext* txn) {}

void ReplicationCoordinatorExternalStateMock::signalApplierToChooseNewSyncSource() {}

void ReplicationCoordinatorExternalStateMock::signalApplierToCancelFetcher() {
    _isApplierSignaledToCancelFetcher = true;
}

OperationContext* ReplicationCoordinatorExternalStateMock::createOperationContext(
    const std::string& threadName) {
    return new OperationContextReplMock;
}

void ReplicationCoordinatorExternalStateMock::dropAllTempCollections(OperationContext* txn) {}

void ReplicationCoordinatorExternalStateMock::dropAllSnapshots() {}

void ReplicationCoordinatorExternalStateMock::updateCommittedSnapshot(SnapshotName newCommitPoint) {
}

void ReplicationCoordinatorExternalStateMock::createSnapshot(OperationContext* txn,
                                                             SnapshotName name) {}

void ReplicationCoordinatorExternalStateMock::forceSnapshotCreation() {}

bool ReplicationCoordinatorExternalStateMock::snapshotsEnabled() const {
    return _areSnapshotsEnabled;
}

void ReplicationCoordinatorExternalStateMock::setAreSnapshotsEnabled(bool val) {
    _areSnapshotsEnabled = val;
}

void ReplicationCoordinatorExternalStateMock::notifyOplogMetadataWaiters() {}

double ReplicationCoordinatorExternalStateMock::getElectionTimeoutOffsetLimitFraction() const {
    return 0.15;
}

bool ReplicationCoordinatorExternalStateMock::isReadCommittedSupportedByStorageEngine(
    OperationContext* txn) const {
    return _isReadCommittedSupported;
}

void ReplicationCoordinatorExternalStateMock::setIsReadCommittedEnabled(bool val) {
    _isReadCommittedSupported = val;
}

OpTime ReplicationCoordinatorExternalStateMock::onTransitionToPrimary(OperationContext* txn,
                                                                      bool isV1ElectionProtocol) {
    if (isV1ElectionProtocol) {
        _lastOpTime = OpTime(Timestamp(1, 0), 1);
    }
    return fassertStatusOK(40297, _lastOpTime);
}

}  // namespace repl
}  // namespace mongo
