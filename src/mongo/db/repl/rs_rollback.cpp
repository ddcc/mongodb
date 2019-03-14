/* @file rs_rollback.cpp
*
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#include "mongo/db/repl/rs_rollback.h"

#include <algorithm>
#include <memory>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

/* Scenarios
 *
 * We went offline with ops not replicated out.
 *
 *     F = node that failed and coming back.
 *     P = node that took over, new primary
 *
 * #1:
 *     F : a b c d e f g
 *     P : a b c d q
 *
 * The design is "keep P".  One could argue here that "keep F" has some merits, however, in most
 * cases P will have significantly more data.  Also note that P may have a proper subset of F's
 * stream if there were no subsequent writes.
 *
 * For now the model is simply : get F back in sync with P.  If P was really behind or something, we
 * should have just chosen not to fail over anyway.
 *
 * #2:
 *     F : a b c d e f g                -> a b c d
 *     P : a b c d
 *
 * #3:
 *     F : a b c d e f g                -> a b c d q r s t u v w x z
 *     P : a b c d.q r s t u v w x z
 *
 * Steps
 *  find an event in common. 'd'.
 *  undo our events beyond that by:
 *    (1) taking copy from other server of those objects
 *    (2) do not consider copy valid until we pass reach an optime after when we fetched the new
 *        version of object
 *        -- i.e., reset minvalid.
 *    (3) we could skip operations on objects that are previous in time to our capture of the object
 *        as an optimization.
 *
 */

namespace mongo {

using std::shared_ptr;
using std::unique_ptr;
using std::endl;
using std::list;
using std::map;
using std::multimap;
using std::set;
using std::string;
using std::pair;

namespace repl {

// Failpoint which causes rollback to hang before finishing.
MONGO_FP_DECLARE(rollbackHangBeforeFinish);
MONGO_FP_DECLARE(rollbackHangThenFailAfterWritingMinValid);

using namespace rollback_internal;

bool DocID::operator<(const DocID& other) const {
    int comp = strcmp(ns, other.ns);
    if (comp < 0)
        return true;
    if (comp > 0)
        return false;

    return _id < other._id;
}

bool DocID::operator==(const DocID& other) const {
    // Since this is only used for tests, going with the simple impl that reuses operator< which is
    // used in the real code.
    return !(*this < other || other < *this);
}

void FixUpInfo::removeAllDocsToRefetchFor(const std::string& collection) {
    docsToRefetch.erase(docsToRefetch.lower_bound(DocID::minFor(collection.c_str())),
                        docsToRefetch.upper_bound(DocID::maxFor(collection.c_str())));
}

void FixUpInfo::removeRedundantOperations() {
    // These loops and their bodies can be done in any order. The final result of the FixUpInfo
    // members will be the same either way.
    for (const auto& collection : collectionsToDrop) {
        removeAllDocsToRefetchFor(collection);
        indexesToDrop.erase(collection);
        collectionsToResyncMetadata.erase(collection);
    }

    for (const auto& collection : collectionsToResyncData) {
        removeAllDocsToRefetchFor(collection);
        indexesToDrop.erase(collection);
        collectionsToResyncMetadata.erase(collection);
        collectionsToDrop.erase(collection);
    }
}

Status rollback_internal::updateFixUpInfoFromLocalOplogEntry(FixUpInfo& fixUpInfo,
                                                             const BSONObj& ourObj) {
    const char* op = ourObj.getStringField("op");
    if (*op == 'n')
        return Status::OK();

    if (ourObj.objsize() > 512 * 1024 * 1024)
        throw RSFatalException("rollback too large");

    DocID doc;
    doc.ownedObj = ourObj.getOwned();
    doc.ns = doc.ownedObj.getStringField("ns");
    if (*doc.ns == '\0') {
        throw RSFatalException(str::stream()
                               << "local op on rollback has no ns: " << doc.ownedObj.toString());
    }

    BSONObj obj = doc.ownedObj.getObjectField(*op == 'u' ? "o2" : "o");
    if (obj.isEmpty()) {
        throw RSFatalException(str::stream()
                               << "local op on rollback has no object field: " << (doc.ownedObj));
    }

    if (*op == 'c') {
        BSONElement first = obj.firstElement();
        NamespaceString nss(doc.ns);  // foo.$cmd
        string cmdname = first.fieldName();
        Command* cmd = Command::findCommand(cmdname.c_str());
        if (cmd == NULL) {
            severe() << "rollback no such command " << first.fieldName();
            return Status(ErrorCodes::UnrecoverableRollbackError,
                          str::stream() << "rollback no such command " << first.fieldName(),
                          18751);
        }
        if (cmdname == "create") {
            // Create collection operation
            // { ts: ..., h: ..., op: "c", ns: "foo.$cmd", o: { create: "abc", ... } }
            string ns = nss.db().toString() + '.' + obj["create"].String();  // -> foo.abc
            fixUpInfo.collectionsToDrop.insert(ns);
            return Status::OK();
        } else if (cmdname == "drop") {
            string ns = nss.db().toString() + '.' + first.valuestr();
            fixUpInfo.collectionsToResyncData.insert(ns);
            return Status::OK();
        } else if (cmdname == "dropIndexes" || cmdname == "deleteIndexes") {
            // TODO: this is bad.  we simply full resync the collection here,
            //       which could be very slow.
            warning() << "rollback of dropIndexes is slow in this version of "
                      << "mongod";
            string ns = nss.db().toString() + '.' + first.valuestr();
            fixUpInfo.collectionsToResyncData.insert(ns);
            return Status::OK();
        } else if (cmdname == "renameCollection") {
            // TODO: slow.
            warning() << "rollback of renameCollection is slow in this version of "
                      << "mongod";
            string from = first.valuestr();
            string to = obj["to"].String();
            fixUpInfo.collectionsToResyncData.insert(from);
            fixUpInfo.collectionsToResyncData.insert(to);
            return Status::OK();
        } else if (cmdname == "dropDatabase") {
            severe() << "rollback : can't rollback drop database full resync "
                     << "will be required";
            log() << obj.toString();
            throw RSFatalException();
        } else if (cmdname == "collMod") {
            const auto ns = NamespaceString(cmd->parseNs(nss.db().toString(), obj));
            for (auto field : obj) {
                const auto modification = field.fieldNameStringData();
                if (modification == cmdname) {
                    continue;  // Skipping command name.
                }

                if (modification == "validator" || modification == "validationAction" ||
                    modification == "validationLevel" || modification == "usePowerOf2Sizes" ||
                    modification == "noPadding") {
                    fixUpInfo.collectionsToResyncMetadata.insert(ns.ns());
                    continue;
                }

                severe() << "cannot rollback a collMod command: " << obj;
                throw RSFatalException();
            }
            return Status::OK();
        } else if (cmdname == "applyOps") {
            if (first.type() != Array) {
                std::string message = str::stream()
                    << "Expected applyOps argument to be an array; found " << first.toString();
                severe() << message;
                return Status(ErrorCodes::UnrecoverableRollbackError, message);
            }
            for (const auto& subopElement : first.Array()) {
                if (subopElement.type() != Object) {
                    std::string message = str::stream()
                        << "Expected applyOps operations to be of Object type, but found "
                        << subopElement.toString();
                    severe() << message;
                    return Status(ErrorCodes::UnrecoverableRollbackError, message);
                }
                auto subStatus = updateFixUpInfoFromLocalOplogEntry(fixUpInfo, subopElement.Obj());
                if (!subStatus.isOK()) {
                    return subStatus;
                }
            }
            return Status::OK();
        } else {
            severe() << "can't rollback this command yet: " << obj.toString();
            log() << "cmdname=" << cmdname;
            throw RSFatalException();
        }
    }

    NamespaceString nss(doc.ns);
    if (nss.isSystemDotIndexes()) {
        if (*op != 'i') {
            severe() << "Unexpected operation type '" << *op << "' on system.indexes operation, "
                     << "document: " << doc.ownedObj;
            throw RSFatalException();
        }
        string objNs;
        auto status = bsonExtractStringField(obj, "ns", &objNs);
        if (!status.isOK()) {
            severe() << "Missing collection namespace in system.indexes operation, document: "
                     << doc.ownedObj;
            throw RSFatalException();
        }
        NamespaceString objNss(objNs);
        if (!objNss.isValid()) {
            severe() << "Invalid collection namespace in system.indexes operation, document: "
                     << doc.ownedObj;
            throw RSFatalException();
        }
        string indexName;
        status = bsonExtractStringField(obj, "name", &indexName);
        if (!status.isOK()) {
            severe() << "Missing index name in system.indexes operation, document: "
                     << doc.ownedObj;
            throw RSFatalException();
        }
        using ValueType = multimap<string, string>::value_type;
        ValueType pairToInsert = std::make_pair(objNs, indexName);
        auto lowerBound = fixUpInfo.indexesToDrop.lower_bound(objNs);
        auto upperBound = fixUpInfo.indexesToDrop.upper_bound(objNs);
        auto matcher = [pairToInsert](const ValueType& pair) { return pair == pairToInsert; };
        if (!std::count_if(lowerBound, upperBound, matcher)) {
            fixUpInfo.indexesToDrop.insert(pairToInsert);
        }
        return Status::OK();
    }

    doc._id = obj["_id"];
    if (doc._id.eoo()) {
        severe() << "cannot rollback op with no _id. ns: " << doc.ns
                 << ", document: " << doc.ownedObj;
        throw RSFatalException();
    }

    fixUpInfo.docsToRefetch.insert(doc);
    return Status::OK();
}

namespace {

/**
 * This must be called before making any changes to our local data and after fetching any
 * information from the upstream node. If any information is fetched from the upstream node after we
 * have written locally, the function must be called again.
 */
void checkRbidAndUpdateMinValid(OperationContext* txn,
                                const int rbid,
                                const RollbackSource& rollbackSource) {
    // It is important that the steps are performed in order to avoid racing with upstream rollbacks
    //
    // 1) Get the last doc in their oplog.
    // 2) Get their RBID and fail if it has changed.
    // 3) Set our minValid to the previously fetched OpTime of the top of their oplog.

    const auto newMinValidDoc = rollbackSource.getLastOperation();
    if (newMinValidDoc.isEmpty()) {
        uasserted(40361, "rollback error newest oplog entry on source is missing or empty");
    }
    if (rbid != rollbackSource.getRollbackId()) {
        // Our source rolled back itself so the data we received isn't necessarily consistent.
        uasserted(40365, "rollback rbid on source changed during rollback, canceling this attempt");
    }

    // we have items we are writing that aren't from a point-in-time.  thus best not to come
    // online until we get to that point in freshness.
    OpTime minValid = fassertStatusOK(28774, OpTime::parseFromOplogEntry(newMinValidDoc));
    log() << "Setting minvalid to " << minValid;
    setAppliedThrough(txn, {});  // Use top of oplog.
    setMinValid(txn, minValid);

    if (MONGO_FAIL_POINT(rollbackHangThenFailAfterWritingMinValid)) {
        // This log output is used in js tests so please leave it.
        log() << "rollback - rollbackHangThenFailAfterWritingMinValid fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(rollbackHangThenFailAfterWritingMinValid)) {
            invariant(!inShutdown());  // It is an error to shutdown while enabled.
            mongo::sleepsecs(1);
        }
        uasserted(40378,
                  "failing rollback due to rollbackHangThenFailAfterWritingMinValid fail point");
    }
}

void syncFixUp(OperationContext* txn,
               const FixUpInfo& fixUpInfo,
               const RollbackSource& rollbackSource,
               ReplicationCoordinator* replCoord) {
    // fetch all first so we needn't handle interruption in a fancy way

    unsigned long long totalSize = 0;

    // namespace -> doc id -> doc
    map<string, map<DocID, BSONObj>> goodVersions;

    // fetch all the goodVersions of each document from current primary
    unsigned long long numFetched = 0;
    for (auto&& doc : fixUpInfo.docsToRefetch) {
        invariant(!doc._id.eoo());  // This is checked when we insert to the set.

        try {
            // TODO : slow.  lots of round trips.
            numFetched++;
            BSONObj good = rollbackSource.findOne(NamespaceString(doc.ns), doc._id.wrap());
            totalSize += good.objsize();
            if (totalSize >= 300 * 1024 * 1024) {
                throw RSFatalException("replSet too much data to roll back");
            }

            // Note good might be empty, indicating we should delete it.
            goodVersions[doc.ns][doc] = good;
        } catch (const DBException& ex) {
            log() << "rollback couldn't re-get from ns: " << doc.ns << " _id: " << (doc._id) << ' '
                  << numFetched << '/' << fixUpInfo.docsToRefetch.size() << ": " << (ex);
            throw;
        }
    }

    log() << "rollback 3.5";
    checkRbidAndUpdateMinValid(txn, fixUpInfo.rbid, rollbackSource);

    // update them
    log() << "rollback 4 n:" << goodVersions.size();

    invariant(!fixUpInfo.commonPointOurDiskloc.isNull());

    // any full collection resyncs required?
    if (!fixUpInfo.collectionsToResyncData.empty() ||
        !fixUpInfo.collectionsToResyncMetadata.empty()) {
        for (const string& ns : fixUpInfo.collectionsToResyncData) {
            log() << "rollback 4.1.1 coll resync " << ns;

            invariant(!fixUpInfo.indexesToDrop.count(ns));
            invariant(!fixUpInfo.collectionsToResyncMetadata.count(ns));

            const NamespaceString nss(ns);


            {
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
                Database* db = dbHolder().openDb(txn, nss.db().toString());
                invariant(db);
                WriteUnitOfWork wunit(txn);
                fassertStatusOK(40359, db->dropCollectionEvenIfSystem(txn, nss));
                wunit.commit();
            }

            rollbackSource.copyCollectionFromRemote(txn, nss);
        }

        for (const string& ns : fixUpInfo.collectionsToResyncMetadata) {
            log() << "rollback 4.1.2 coll metadata resync " << ns;

            const NamespaceString nss(ns);
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
            auto db = dbHolder().openDb(txn, nss.db().toString());
            invariant(db);
            auto collection = db->getCollection(ns);
            invariant(collection);
            auto cce = collection->getCatalogEntry();

            auto infoResult = rollbackSource.getCollectionInfo(nss);

            if (!infoResult.isOK()) {
                // Collection dropped by "them" so we can't correctly change it here. If we get to
                // the roll-forward phase, we will drop it then. If the drop is rolled-back upstream
                // and we restart, we will be expected to still have the collection.
                log() << ns << " not found on remote host, so not rolling back collmod operation."
                               " We will drop the collection soon.";
                continue;
            }

            auto info = infoResult.getValue();
            CollectionOptions options;
            if (auto optionsField = info["options"]) {
                if (optionsField.type() != Object) {
                    throw RSFatalException(str::stream() << "Failed to parse options " << info
                                                         << ": expected 'options' to be an "
                                                         << "Object, got "
                                                         << typeName(optionsField.type()));
                }

                auto status = options.parse(optionsField.Obj());
                if (!status.isOK()) {
                    throw RSFatalException(str::stream() << "Failed to parse options " << info
                                                         << ": " << status.toString());
                }
            } else {
                // Use default options.
            }

            WriteUnitOfWork wuow(txn);
            if (options.flagsSet || cce->getCollectionOptions(txn).flagsSet) {
                cce->updateFlags(txn, options.flags);
            }

            auto status = collection->setValidator(txn, options.validator);
            if (!status.isOK()) {
                throw RSFatalException(str::stream()
                                       << "Failed to set validator: " << status.toString());
            }
            status = collection->setValidationAction(txn, options.validationAction);
            if (!status.isOK()) {
                throw RSFatalException(str::stream()
                                       << "Failed to set validationAction: " << status.toString());
            }

            status = collection->setValidationLevel(txn, options.validationLevel);
            if (!status.isOK()) {
                throw RSFatalException(str::stream()
                                       << "Failed to set validationLevel: " << status.toString());
            }

            wuow.commit();
        }

        // we did more reading from primary, so check it again for a rollback (which would mess
        // us up), and make minValid newer.
        log() << "rollback 4.2";
        checkRbidAndUpdateMinValid(txn, fixUpInfo.rbid, rollbackSource);
    }

    log() << "rollback 4.6";
    // drop collections to drop before doing individual fixups
    for (set<string>::iterator it = fixUpInfo.collectionsToDrop.begin();
         it != fixUpInfo.collectionsToDrop.end();
         it++) {
        log() << "rollback drop: " << *it;

        invariant(!fixUpInfo.indexesToDrop.count(*it));

        ScopedTransaction transaction(txn, MODE_IX);
        const NamespaceString nss(*it);
        Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
        Database* db = dbHolder().get(txn, nsToDatabaseSubstring(*it));
        if (db) {
            Helpers::RemoveSaver removeSaver("rollback", "", *it);

            // perform a collection scan and write all documents in the collection to disk
            std::unique_ptr<PlanExecutor> exec(InternalPlanner::collectionScan(
                txn, *it, db->getCollection(*it), PlanExecutor::YIELD_AUTO));
            BSONObj curObj;
            PlanExecutor::ExecState execState;
            while (PlanExecutor::ADVANCED == (execState = exec->getNext(&curObj, NULL))) {
                auto status = removeSaver.goingToDelete(curObj);
                if (!status.isOK()) {
                    severe() << "rolling back createCollection on " << *it
                             << " failed to write document to remove saver file: " << status;
                    throw RSFatalException();
                }
            }
            if (execState != PlanExecutor::IS_EOF) {
                if (execState == PlanExecutor::FAILURE &&
                    WorkingSetCommon::isValidStatusMemberObject(curObj)) {
                    Status errorStatus = WorkingSetCommon::getMemberObjectStatus(curObj);
                    severe() << "rolling back createCollection on " << *it << " failed with "
                             << errorStatus << ". A full resync is necessary.";
                } else {
                    severe() << "rolling back createCollection on " << *it
                             << " failed. A full resync is necessary.";
                }

                throw RSFatalException();
            }

            WriteUnitOfWork wunit(txn);
            fassertStatusOK(40360, db->dropCollectionEvenIfSystem(txn, nss));
            wunit.commit();
        }
    }

    // Drop indexes.
    for (auto it = fixUpInfo.indexesToDrop.begin(); it != fixUpInfo.indexesToDrop.end(); it++) {
        const NamespaceString nss(it->first);
        const string& indexName = it->second;
        log() << "rollback drop index: collection: " << nss.toString() << ". index: " << indexName;

        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
        auto db = dbHolder().get(txn, nss.db());
        if (!db) {
            continue;
        }
        auto collection = db->getCollection(nss.toString());
        if (!collection) {
            continue;
        }
        auto indexCatalog = collection->getIndexCatalog();
        if (!indexCatalog) {
            continue;
        }
        bool includeUnfinishedIndexes = false;
        auto indexDescriptor =
            indexCatalog->findIndexByName(txn, indexName, includeUnfinishedIndexes);
        if (!indexDescriptor) {
            warning() << "rollback failed to drop index " << indexName << " in " << nss.toString()
                      << ": index not found";
            continue;
        }
        WriteUnitOfWork wunit(txn);
        auto status = indexCatalog->dropIndex(txn, indexDescriptor);
        if (!status.isOK()) {
            severe() << "rollback failed to drop index " << indexName << " in " << nss.toString()
                     << ": " << status;
            throw RSFatalException();
        }
        wunit.commit();
    }

    log() << "rollback 4.7";
    unsigned deletes = 0, updates = 0;
    time_t lastProgressUpdate = time(0);
    time_t progressUpdateGap = 10;
    for (const auto& nsAndGoodVersionsByDocID : goodVersions) {
        // Keep an archive of items rolled back if the collection has not been dropped
        // while rolling back createCollection operations.
        const auto& ns = nsAndGoodVersionsByDocID.first;
        unique_ptr<Helpers::RemoveSaver> removeSaver;
        invariant(!fixUpInfo.collectionsToDrop.count(ns));
        removeSaver.reset(new Helpers::RemoveSaver("rollback", "", ns));

        const auto& goodVersionsByDocID = nsAndGoodVersionsByDocID.second;
        for (const auto& idAndDoc : goodVersionsByDocID) {
            time_t now = time(0);
            if (now - lastProgressUpdate > progressUpdateGap) {
                log() << deletes << " delete and " << updates
                      << " update operations processed out of " << goodVersions.size()
                      << " total operations";
                lastProgressUpdate = now;
            }
            const DocID& doc = idAndDoc.first;
            BSONObj pattern = doc._id.wrap();  // { _id : ... }
            try {
                verify(doc.ns && *doc.ns);
                invariant(!fixUpInfo.collectionsToResyncData.count(doc.ns));

                // TODO: Lots of overhead in context. This can be faster.
                const NamespaceString docNss(doc.ns);
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock docDbLock(txn->lockState(), docNss.db(), MODE_X);
                OldClientContext ctx(txn, doc.ns);

                Collection* collection = ctx.db()->getCollection(doc.ns);

                // Add the doc to our rollback file if the collection was not dropped while
                // rolling back createCollection operations.
                // Do not log an error when undoing an insert on a no longer existent
                // collection.
                // It is likely that the collection was dropped as part of rolling back a
                // createCollection command and regardless, the document no longer exists.
                if (collection && removeSaver) {
                    BSONObj obj;
                    bool found = Helpers::findOne(txn, collection, pattern, obj, false);
                    if (found) {
                        auto status = removeSaver->goingToDelete(obj);
                        if (!status.isOK()) {
                            severe() << "rollback cannot write document in namespace " << doc.ns
                                     << " to archive file: " << status;
                            throw RSFatalException();
                        }
                    } else {
                        error() << "rollback cannot find object: " << pattern << " in namespace "
                                << doc.ns;
                    }
                }

                if (idAndDoc.second.isEmpty()) {
                    // wasn't on the primary; delete.
                    // TODO 1.6 : can't delete from a capped collection.  need to handle that
                    // here.
                    deletes++;

                    if (collection) {
                        if (collection->isCapped()) {
                            // can't delete from a capped collection - so we truncate instead.
                            // if
                            // this item must go, so must all successors!!!
                            try {
                                // TODO: IIRC cappedTruncateAfter does not handle completely
                                // empty.
                                // this will crazy slow if no _id index.
                                long long start = Listener::getElapsedTimeMillis();
                                RecordId loc = Helpers::findOne(txn, collection, pattern, false);
                                if (Listener::getElapsedTimeMillis() - start > 200)
                                    warning() << "roll back slow no _id index for " << doc.ns
                                              << " perhaps?";
                                // would be faster but requires index:
                                // RecordId loc = Helpers::findById(nsd, pattern);
                                if (!loc.isNull()) {
                                    try {
                                        collection->temp_cappedTruncateAfter(txn, loc, true);
                                    } catch (const DBException& e) {
                                        if (e.getCode() == 13415) {
                                            // hack: need to just make cappedTruncate do this...
                                            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                                                WriteUnitOfWork wunit(txn);
                                                uassertStatusOK(collection->truncate(txn));
                                                wunit.commit();
                                            }
                                            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                                                txn, "truncate", collection->ns().ns());
                                        } else {
                                            throw e;
                                        }
                                    }
                                }
                            } catch (const DBException& e) {
                                // Replicated capped collections have many ways to become
                                // inconsistent. We rely on age-out to make these problems go away
                                // eventually.
                                warning() << "ignoring failure to roll back change to capped "
                                          << "collection " << doc.ns << " with _id "
                                          << idAndDoc.first._id.toString(
                                                 /*includeFieldName*/ false) << ": " << e;
                            }
                        } else {
                            deleteObjects(txn,
                                          collection,
                                          doc.ns,
                                          pattern,
                                          PlanExecutor::YIELD_MANUAL,
                                          true,   // justone
                                          true);  // god
                        }
                    }
                } else {
                    // TODO faster...
                    OpDebug debug;
                    updates++;

                    const NamespaceString requestNs(doc.ns);
                    UpdateRequest request(requestNs);

                    request.setQuery(pattern);
                    request.setUpdates(idAndDoc.second);
                    request.setGod();
                    request.setUpsert();
                    UpdateLifecycleImpl updateLifecycle(true, requestNs);
                    request.setLifecycle(&updateLifecycle);

                    update(txn, ctx.db(), request, &debug);
                }
            } catch (const DBException& e) {
                log() << "exception in rollback ns:" << doc.ns << ' ' << pattern.toString() << ' '
                      << e << " ndeletes:" << deletes;
                throw;
            }
        }
    }

    log() << "rollback 5 d:" << deletes << " u:" << updates;
    log() << "rollback 6";

    // clean up oplog
    LOG(2) << "rollback truncate oplog after " << fixUpInfo.commonPoint;
    {
        const NamespaceString oplogNss(rsOplogName);
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock oplogDbLock(txn->lockState(), oplogNss.db(), MODE_IX);
        Lock::CollectionLock oplogCollectionLoc(txn->lockState(), oplogNss.ns(), MODE_X);
        OldClientContext ctx(txn, rsOplogName);
        Collection* oplogCollection = ctx.db()->getCollection(rsOplogName);
        if (!oplogCollection) {
            fassertFailedWithStatusNoTrace(13423,
                                           Status(ErrorCodes::UnrecoverableRollbackError,
                                                  str::stream() << "Can't find " << rsOplogName));
        }
        // TODO: fatal error if this throws?
        oplogCollection->temp_cappedTruncateAfter(txn, fixUpInfo.commonPointOurDiskloc, false);
    }

    Status status = getGlobalAuthorizationManager()->initialize(txn);
    if (!status.isOK()) {
        severe() << "Failed to reinitialize auth data after rollback: " << status;
        fassertFailedNoTrace(40366);
    }

    // Reload the lastAppliedOpTime and lastDurableOpTime value in the replcoord and the
    // lastAppliedHash value in bgsync to reflect our new last op.
    replCoord->resetLastOpTimesFromOplog(txn);
    log() << "rollback done";
}

Status _syncRollback(OperationContext* txn,
                     const OplogInterface& localOplog,
                     const RollbackSource& rollbackSource,
                     boost::optional<int> requiredRBID,
                     ReplicationCoordinator* replCoord) {
    invariant(!txn->lockState()->isLocked());

    FixUpInfo how;
    log() << "rollback 1";
    how.rbid = rollbackSource.getRollbackId();
    if (requiredRBID) {
        uassert(40362,
                "Upstream node rolled back. Need to retry our rollback.",
                how.rbid == *requiredRBID);
    }

    log() << "rollback 2 FindCommonPoint";
    try {
        auto processOperationForFixUp = [&how](const BSONObj& operation) {
            return updateFixUpInfoFromLocalOplogEntry(how, operation);
        };
        auto res = syncRollBackLocalOperations(
            localOplog, rollbackSource.getOplog(), processOperationForFixUp);
        if (!res.isOK()) {
            const auto status = res.getStatus();
            switch (status.code()) {
                case ErrorCodes::OplogStartMissing:
                case ErrorCodes::UnrecoverableRollbackError:
                    return status;
                default:
                    throw RSFatalException(status.toString());
            }
        }

        how.commonPoint = res.getValue().first;
        how.commonPointOurDiskloc = res.getValue().second;
        how.removeRedundantOperations();
    } catch (const RSFatalException& e) {
        return Status(ErrorCodes::UnrecoverableRollbackError,
                      str::stream()
                          << "need to rollback, but unable to determine common point between"
                             " local and remote oplog: " << e.what(),
                      18752);
    }

    log() << "rollback common point is " << how.commonPoint;
    log() << "rollback 3 fixup";
    try {
        ON_BLOCK_EXIT([&] { replCoord->incrementRollbackID(); });
        syncFixUp(txn, how, rollbackSource, replCoord);
    } catch (const RSFatalException& e) {
        return Status(ErrorCodes::UnrecoverableRollbackError, e.what(), 18753);
    }

    if (MONGO_FAIL_POINT(rollbackHangBeforeFinish)) {
        // This log output is used in js tests so please leave it.
        log() << "rollback - rollbackHangBeforeFinish fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(rollbackHangBeforeFinish)) {
            invariant(!inShutdown());  // It is an error to shutdown while enabled.
            mongo::sleepsecs(1);
        }
    }

    return Status::OK();
}

}  // namespace

Status syncRollback(OperationContext* txn,
                    const OplogInterface& localOplog,
                    const RollbackSource& rollbackSource,
                    boost::optional<int> requiredRBID,
                    ReplicationCoordinator* replCoord) {
    invariant(txn);
    invariant(replCoord);

    log() << "beginning rollback" << rsLog;

    DisableDocumentValidation validationDisabler(txn);
    txn->setReplicatedWrites(false);
    Status status = _syncRollback(txn, localOplog, rollbackSource, requiredRBID, replCoord);

    log() << "rollback finished" << rsLog;
    return status;
}

}  // namespace repl
}  // namespace mongo
