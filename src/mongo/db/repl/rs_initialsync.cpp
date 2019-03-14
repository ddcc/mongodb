/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rs_initialsync.h"

#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/cloner.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/initial_sync.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

using std::list;
using std::string;

// Failpoint which fails initial sync and leaves on oplog entry in the buffer.
MONGO_FP_DECLARE(failInitSyncWithBufferedEntriesLeft);

// Failpoint which causes the initial sync function to hang before copying databases.
MONGO_FP_DECLARE(initialSyncHangBeforeCopyingDatabases);


/**
 * Truncates the oplog (removes any documents) and resets internal variables that were
 * originally initialized or affected by using values from the oplog at startup time.  These
 * include the last applied optime, the last fetched optime, and the sync source blacklist.
 * Also resets the bgsync thread so that it reconnects its sync source after the oplog has been
 * truncated.
 */
void truncateAndResetOplog(OperationContext* txn,
                           ReplicationCoordinator* replCoord,
                           BackgroundSync* bgsync) {
    // Add field to minvalid document to tell us to restart initial sync if we crash
    setInitialSyncFlag(txn);

    AutoGetDb autoDb(txn, "local", MODE_X);
    massert(28585, "no local database found", autoDb.getDb());
    invariant(txn->lockState()->isCollectionLockedForMode(rsOplogName, MODE_X));
    // Note: the following order is important.
    // The bgsync thread uses an empty optime as a sentinel to know to wait
    // for initial sync; thus, we must
    // ensure the lastAppliedOpTime is empty before restarting the bgsync thread
    // via stop().
    // We must clear the sync source blacklist after calling stop()
    // because the bgsync thread, while running, may update the blacklist.
    replCoord->resetMyLastOpTimes();
    bgsync->stop();
    bgsync->clearBuffer();

    replCoord->clearSyncSourceBlacklist();

    // Truncate the oplog in case there was a prior initial sync that failed.
    Collection* collection = autoDb.getDb()->getCollection(rsOplogName);
    fassert(28565, collection);
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        WriteUnitOfWork wunit(txn);
        Status status = collection->truncate(txn);
        fassert(28564, status);
        wunit.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "truncate", collection->ns().ns());
}

/**
 * Confirms that the "admin" database contains a supported version of the auth
 * data schema.  Terminates the process if the "admin" contains clearly incompatible
 * auth data.
 */
void checkAdminDatabasePostClone(OperationContext* txn, Database* adminDb) {
    // Assumes txn holds MODE_X or MODE_S lock on "admin" database.
    if (!adminDb) {
        return;
    }
    Collection* const usersCollection =
        adminDb->getCollection(AuthorizationManager::usersCollectionNamespace);
    const bool hasUsers =
        usersCollection && !Helpers::findOne(txn, usersCollection, BSONObj(), false).isNull();
    Collection* const adminVersionCollection =
        adminDb->getCollection(AuthorizationManager::versionCollectionNamespace);
    BSONObj authSchemaVersionDocument;
    if (!adminVersionCollection ||
        !Helpers::findOne(txn,
                          adminVersionCollection,
                          AuthorizationManager::versionDocumentQuery,
                          authSchemaVersionDocument)) {
        if (!hasUsers) {
            // It's OK to have no auth version document if there are no user documents.
            return;
        }
        severe() << "During initial sync, found documents in "
                 << AuthorizationManager::usersCollectionNamespace
                 << " but could not find an auth schema version document in "
                 << AuthorizationManager::versionCollectionNamespace;
        severe() << "This indicates that the primary of this replica set was not successfully "
                    "upgraded to schema version " << AuthorizationManager::schemaVersion26Final
                 << ", which is the minimum supported schema version in this version of MongoDB";
        fassertFailedNoTrace(28620);
    }
    long long foundSchemaVersion;
    Status status = bsonExtractIntegerField(authSchemaVersionDocument,
                                            AuthorizationManager::schemaVersionFieldName,
                                            &foundSchemaVersion);
    if (!status.isOK()) {
        severe() << "During initial sync, found malformed auth schema version document: " << status
                 << "; document: " << authSchemaVersionDocument;
        fassertFailedNoTrace(28618);
    }
    if ((foundSchemaVersion != AuthorizationManager::schemaVersion26Final) &&
        (foundSchemaVersion != AuthorizationManager::schemaVersion28SCRAM)) {
        severe() << "During initial sync, found auth schema version " << foundSchemaVersion
                 << ", but this version of MongoDB only supports schema versions "
                 << AuthorizationManager::schemaVersion26Final << " and "
                 << AuthorizationManager::schemaVersion28SCRAM;
        fassertFailedNoTrace(28619);
    }
}

bool _initialSyncClone(OperationContext* txn,
                       Cloner& cloner,
                       const std::string& host,
                       const std::string& db,
                       std::vector<BSONObj>& collections,
                       bool dataPass) {
    if (db == "local")
        return true;

    if (dataPass)
        log() << "initial sync cloning db: " << db;
    else
        log() << "initial sync cloning indexes for : " << db;

    CloneOptions options;
    options.fromDB = db;
    options.slaveOk = true;
    options.useReplAuth = true;
    options.snapshot = false;
    options.syncData = dataPass;
    options.syncIndexes = !dataPass;
    options.createCollections = false;

    // Make database stable
    ScopedTransaction transaction(txn, MODE_IX);
    Lock::DBLock dbWrite(txn->lockState(), db, MODE_X);

    Status status = cloner.copyDb(txn, db, host, options, nullptr, collections);
    if (!status.isOK()) {
        log() << "initial sync: error while " << (dataPass ? "cloning " : "indexing ") << db
              << ".  " << status.toString();
        return false;
    }

    if (dataPass && (db == "admin")) {
        checkAdminDatabasePostClone(txn, dbHolder().get(txn, db));
    }
    return true;
}

/**
 * Replays the sync target's oplog from lastOp to the latest op on the sync target.
 *
 * @param syncer used to apply the oplog (from the reader).
 * @param r      the oplog reader.
 * @return if applying the oplog succeeded.
 */
bool _initialSyncApplyOplog(OperationContext* ctx, repl::InitialSync* syncer, OplogReader* r) {
    const OpTime startOpTime = getGlobalReplicationCoordinator()->getMyLastAppliedOpTime();
    BSONObj lastOp;

    // If the fail point is set, exit failing.
    if (MONGO_FAIL_POINT(failInitSyncWithBufferedEntriesLeft)) {
        log() << "adding fake oplog entry to buffer.";
        BackgroundSync::get()->pushTestOpToBuffer(BSON(
            "ts" << startOpTime.getTimestamp() << "t" << startOpTime.getTerm() << "v" << 1 << "op"
                 << "n"));
        return false;
    }

    try {
        // It may have been a long time since we last used this connection to
        // query the oplog, depending on the size of the databases we needed to clone.
        // A common problem is that TCP keepalives are set too infrequent, and thus
        // our connection here is terminated by a firewall due to inactivity.
        // Solution is to increase the TCP keepalive frequency.
        lastOp = r->getLastOp(rsOplogName);
    } catch (SocketException&) {
        HostAndPort host = r->getHost();
        log() << "connection lost to " << host.toString()
              << "; is your tcp keepalive interval set appropriately?";
        if (!r->connect(host)) {
            error() << "initial sync couldn't connect to " << host.toString();
            throw;
        }
        // retry
        lastOp = r->getLastOp(rsOplogName);
    }

    if (lastOp.isEmpty()) {
        error() << "initial sync lastOp is empty";
        sleepsecs(1);
        return false;
    }

    OpTime stopOpTime = fassertStatusOK(28777, OpTime::parseFromOplogEntry(lastOp));

    // If we already have what we need then return.
    if (stopOpTime == startOpTime)
        return true;

    verify(!stopOpTime.isNull());
    verify(stopOpTime.getTimestamp() > startOpTime.getTimestamp());

    // apply till stopOpTime
    try {
        LOG(2) << "Applying oplog entries from " << startOpTime << " until " << stopOpTime;
        syncer->oplogApplication(ctx, stopOpTime);

        if (inShutdown()) {
            return false;
        }
    } catch (const DBException&) {
        warning() << "initial sync failed during oplog application phase, and will retry";
        sleepsecs(5);
        return false;
    }

    return true;
}

/**
 * Do the initial sync for this member.  There are several steps to this process:
 *
 *     0. Add _initialSyncFlag to minValid collection to tell us to restart initial sync if we
 *        crash in the middle of this procedure
 *     1. Record start time.
 *     2. Clone.
 *     3. Set minValid1 to sync target's latest op time.
 *     4. Apply ops from start to minValid1, fetching missing docs as needed.
 *     5. Set minValid2 to sync target's latest op time.
 *     6. Apply ops from minValid1 to minValid2.
 *     7. Build indexes.
 *     8. Set minValid3 to sync target's latest op time.
 *     9. Apply ops from minValid2 to minValid3.
      10. Cleanup minValid collection: remove _initialSyncFlag field, set ts to minValid3 OpTime
 *
 * At that point, initial sync is finished.  Note that the oplog from the sync target is applied
 * three times: step 4, 6, and 8.  4 may involve refetching, 6 should not.  By the end of 6,
 * this member should have consistent data.  8 is "cosmetic," it is only to get this member
 * closer to the latest op time before it can transition out of startup state
 *
 * Returns a Status with ErrorCode::ShutdownInProgress if the node enters shutdown,
 * ErrorCode::InitialSyncOplogSourceMissing if the node fails to find an sync source, Status::OK
 * if everything worked, and ErrorCode::InitialSyncFailure for all other error cases.
 */
Status _initialSync() {
    log() << "initial sync pending";

    BackgroundSync* bgsync(BackgroundSync::get());
    OperationContextImpl txn;
    txn.setReplicatedWrites(false);
    DisableDocumentValidation validationDisabler(&txn);
    ReplicationCoordinator* replCoord(getGlobalReplicationCoordinator());

    // reset state for initial sync
    truncateAndResetOplog(&txn, replCoord, bgsync);

    OplogReader r;

    while (r.getHost().empty()) {
        // We must prime the sync source selector so that it considers all candidates regardless
        // of oplog position, by passing in null OpTime as the last op fetched time.
        r.connectToSyncSource(&txn, OpTime(), OpTime(), replCoord);
        if (r.getHost().empty()) {
            std::string msg =
                "no valid sync sources found in current replset to do an initial sync";
            log() << msg;
            return Status(ErrorCodes::InitialSyncOplogSourceMissing, msg);
        }

        if (inShutdown()) {
            return Status(ErrorCodes::ShutdownInProgress, "shutting down");
        }
    }

    InitialSync init(bgsync, multiInitialSyncApply);
    init.setHostname(r.getHost().toString());

    BSONObj lastOp = r.getLastOp(rsOplogName);
    if (lastOp.isEmpty()) {
        std::string msg = "initial sync couldn't read remote oplog";
        log() << msg;
        sleepsecs(15);
        return Status(ErrorCodes::InitialSyncFailure, msg);
    }

    log() << "initial sync drop all databases";
    dropAllDatabasesExceptLocal(&txn);

    log() << "initial sync clone all databases";

    if (MONGO_FAIL_POINT(initialSyncHangBeforeCopyingDatabases)) {
        // This log output is used in js tests so please leave it.
        log() << "initial sync - initialSyncHangBeforeCopyingDatabases fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(initialSyncHangBeforeCopyingDatabases) && !inShutdown()) {
            mongo::sleepsecs(1);
        }
    }

    list<string> dbs = r.conn()->getDatabaseNames();
    {
        // Clone admin database first, to catch schema errors.
        list<string>::iterator admin = std::find(dbs.begin(), dbs.end(), "admin");
        if (admin != dbs.end()) {
            dbs.splice(dbs.begin(), dbs, admin);
        }
        // Ignore local db
        dbs.erase(std::remove(dbs.begin(), dbs.end(), "local"), dbs.end());
    }

    Cloner cloner;
    std::map<std::string, std::vector<BSONObj>> collectionsPerDb;
    for (auto&& db : dbs) {
        CloneOptions options;
        options.fromDB = db;
        log() << "fetching and creating collections for " << db;
        std::list<BSONObj> initialCollections =
            r.conn()->getCollectionInfos(options.fromDB);  // may uassert
        auto fetchStatus = cloner.filterCollectionsForClone(options, initialCollections);
        if (!fetchStatus.isOK()) {
            return fetchStatus.getStatus();
        }
        auto collections = fetchStatus.getValue();

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock dbWrite(txn.lockState(), db, MODE_X);

        auto createStatus = cloner.createCollectionsForDb(&txn, collections, db);
        if (!createStatus.isOK()) {
            return createStatus;
        }
        collectionsPerDb.emplace(db, std::move(collections));
    }
    for (auto&& dbCollsPair : collectionsPerDb) {
        if (!_initialSyncClone(&txn,
                               cloner,
                               r.conn()->getServerAddress(),
                               dbCollsPair.first,
                               dbCollsPair.second,
                               true)) {
            return Status(ErrorCodes::InitialSyncFailure, "initial sync failed data cloning");
        }
    }

    log() << "initial sync data copy, starting syncup";

    // prime oplog, but don't need to actually apply the op as the cloned data already reflects it.
    OpTime lastOptime = writeOpsToOplog(&txn, {lastOp});
    ReplClientInfo::forClient(txn.getClient()).setLastOp(lastOptime);
    replCoord->setMyLastAppliedOpTime(lastOptime);
    setNewTimestamp(lastOptime.getTimestamp());

    std::string msg = "oplog sync 1 of 3";
    log() << msg;
    if (!_initialSyncApplyOplog(&txn, &init, &r)) {
        return Status(ErrorCodes::InitialSyncFailure,
                      str::stream() << "initial sync failed: " << msg);
    }

    // Now we sync to the latest op on the sync target _again_, as we may have recloned ops that
    // were "from the future" from the data clone. During this second application, nothing should
    // need to be recloned.
    // TODO: replace with "tail" instance below, since we don't need to retry/reclone missing docs.
    msg = "oplog sync 2 of 3";
    log() << msg;
    if (!_initialSyncApplyOplog(&txn, &init, &r)) {
        return Status(ErrorCodes::InitialSyncFailure,
                      str::stream() << "initial sync failed: " << msg);
    }
    // data should now be consistent

    msg = "initial sync building indexes";
    log() << msg;
    for (auto&& dbCollsPair : collectionsPerDb) {
        if (!_initialSyncClone(&txn,
                               cloner,
                               r.conn()->getServerAddress(),
                               dbCollsPair.first,
                               dbCollsPair.second,
                               false)) {
            return Status(ErrorCodes::InitialSyncFailure,
                          str::stream() << "initial sync failed: " << msg);
        }
    }

    // WARNING: If the 3rd oplog sync step is removed we must reset minValid to the last entry on
    // the source server so that we don't come out of recovering until we get there (since the
    // previous steps could have fetched newer document than the oplog entry we were applying from).
    msg = "oplog sync 3 of 3";
    log() << msg;

    InitialSync tail(bgsync, multiSyncApply);  // Use the non-initial sync apply code
    if (!_initialSyncApplyOplog(&txn, &tail, &r)) {
        return Status(ErrorCodes::InitialSyncFailure,
                      str::stream() << "initial sync failed: " << msg);
    }

    // ---------

    Status status = getGlobalAuthorizationManager()->initialize(&txn);
    if (!status.isOK()) {
        warning() << "Failed to reinitialize auth data after initial sync. " << status;
        return status;
    }

    log() << "initial sync finishing up";

    // Initial sync is now complete.
    // Clear the initial sync flag -- cannot be done under a db lock, or recursive.
    clearInitialSyncFlag(&txn);
    BackgroundSync::get()->setInitialSyncRequestedFlag(false);

    // Clear maint. mode.
    while (replCoord->getMaintenanceMode()) {
        replCoord->setMaintenanceMode(false);
    }

    log() << "initial sync done";
    return Status::OK();
}
}  // namespace

void syncDoInitialSync() {
    static const int maxFailedAttempts = 10;

    {
        OperationContextImpl txn;
        createOplog(&txn);
    }

    int failedAttempts = 0;
    while (failedAttempts < maxFailedAttempts) {
        try {
            // leave loop when successful
            Status status = _initialSync();
            if (status.isOK()) {
                break;
            }
            if (status == ErrorCodes::InitialSyncOplogSourceMissing) {
                sleepsecs(1);
                return;
            }
        } catch (const DBException& e) {
            error() << e;
            // Return if in shutdown
            if (inShutdown()) {
                return;
            }
        }

        if (inShutdown()) {
            return;
        }

        error() << "initial sync attempt failed, " << (maxFailedAttempts - ++failedAttempts)
                << " attempts remaining";
        sleepsecs(5);
    }

    // No need to print a stack
    if (failedAttempts >= maxFailedAttempts) {
        severe() << "The maximum number of retries have been exhausted for initial sync.";
        fassertFailedNoTrace(16233);
    }

    log() << "initial sync succeeded after " << (failedAttempts + 1) << " attempt(s).";
}

}  // namespace repl
}  // namespace mongo
