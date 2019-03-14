/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::set;
using std::string;
using std::stringstream;

namespace {

StringData _todb(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        uassert(13074, "db name can't be empty", ns.size());
        return ns;
    }

    uassert(13075, "db name can't be empty", i > 0);

    const StringData d = ns.substr(0, i);
    uassert(13280, "invalid db name: " + ns.toString(), NamespaceString::validDBName(d));

    return d;
}


DatabaseHolder _dbHolder;

}  // namespace


DatabaseHolder& dbHolder() {
    return _dbHolder;
}


Database* DatabaseHolder::get(OperationContext* txn, StringData ns) const {
    const StringData db = _todb(ns);
    invariant(txn->lockState()->isDbLockedForMode(db, MODE_IS));

    stdx::lock_guard<SimpleMutex> lk(_m);
    DBs::const_iterator it = _dbs.find(db);
    if (it != _dbs.end()) {
        return it->second;
    }

    return NULL;
}

std::set<std::string> DatabaseHolder::_getNamesWithConflictingCasing_inlock(StringData name) {
    std::set<std::string> duplicates;

    for (const auto& nameAndPointer : _dbs) {
        // A name that's equal with case-insensitive match must be identical, or it's a duplicate.
        if (name.equalCaseInsensitive(nameAndPointer.first) && name != nameAndPointer.first)
            duplicates.insert(nameAndPointer.first);
    }
    return duplicates;
}

std::set<std::string> DatabaseHolder::getNamesWithConflictingCasing(StringData name) {
    stdx::lock_guard<SimpleMutex> lk(_m);
    return _getNamesWithConflictingCasing_inlock(name);
}

Database* DatabaseHolder::openDb(OperationContext* txn, StringData ns, bool* justCreated) {
    const StringData dbname = _todb(ns);
    invariant(txn->lockState()->isDbLockedForMode(dbname, MODE_X));

    if (justCreated)
        *justCreated = false;  // Until proven otherwise.

    stdx::unique_lock<SimpleMutex> lk(_m);

    // The following will insert a nullptr for dbname, which will treated the same as a non-
    // existant database by the get method, yet still counts in getNamesWithConflictingCasing.
    if (auto db = _dbs[dbname])
        return db;

    // We've inserted a nullptr entry for dbname: make sure to remove it on unsuccessful exit.
    auto removeDbGuard = MakeGuard([this, &lk, dbname] {
        if (!lk.owns_lock())
            lk.lock();
        _dbs.erase(dbname);
    });

    // Check casing in lock to avoid transient duplicates.
    auto duplicates = _getNamesWithConflictingCasing_inlock(dbname);
    uassert(ErrorCodes::DatabaseDifferCase,
            str::stream() << "db already exists with different case already have: ["
                          << *duplicates.cbegin() << "] trying to create [" << dbname.toString()
                          << "]",
            duplicates.empty());


    // Do the catalog lookup and database creation outside of the scoped lock, because these may
    // block. Only one thread can be inside this method for the same DB name, because of the
    // requirement for X-lock on the database when we enter. So there is no way we can insert two
    // different databases for the same name.
    lk.unlock();
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    DatabaseCatalogEntry* entry = storageEngine->getDatabaseCatalogEntry(txn, dbname);

    if (!entry->exists()) {
        audit::logCreateDatabase(&cc(), dbname);
        if (justCreated)
            *justCreated = true;
    }

    auto newDb = stdx::make_unique<Database>(txn, dbname, entry);

    // Finally replace our nullptr entry with the new Database pointer.
    removeDbGuard.Dismiss();
    lk.lock();
    auto it = _dbs.find(dbname);
    invariant(it != _dbs.end() && it->second == nullptr);
    Database* newDbPointer = newDb.release();
    _dbs[dbname] = newDbPointer;
    invariant(_getNamesWithConflictingCasing_inlock(dbname.toString()).empty());

    return newDbPointer;
}

void DatabaseHolder::close(OperationContext* txn, StringData ns) {
    invariant(txn->lockState()->isW());

    const StringData dbName = _todb(ns);

    stdx::lock_guard<SimpleMutex> lk(_m);

    DBs::const_iterator it = _dbs.find(dbName);
    if (it == _dbs.end()) {
        return;
    }

    it->second->close(txn);
    delete it->second;
    _dbs.erase(it);

    getGlobalServiceContext()->getGlobalStorageEngine()->closeDatabase(txn, dbName.toString());
}

bool DatabaseHolder::closeAll(OperationContext* txn, BSONObjBuilder& result, bool force) {
    invariant(txn->lockState()->isW());

    stdx::lock_guard<SimpleMutex> lk(_m);

    set<string> dbs;
    for (DBs::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
        dbs.insert(i->first);
    }

    BSONArrayBuilder bb(result.subarrayStart("dbs"));
    int nNotClosed = 0;
    for (set<string>::iterator i = dbs.begin(); i != dbs.end(); ++i) {
        string name = *i;

        LOG(2) << "DatabaseHolder::closeAll name:" << name;

        if (!force && BackgroundOperation::inProgForDb(name)) {
            log() << "WARNING: can't close database " << name
                  << " because a bg job is in progress - try killOp command";
            nNotClosed++;
            continue;
        }

        Database* db = _dbs[name];
        db->close(txn);
        delete db;

        _dbs.erase(name);

        getGlobalServiceContext()->getGlobalStorageEngine()->closeDatabase(txn, name);

        bb.append(name);
    }

    bb.done();
    if (nNotClosed) {
        result.append("nNotClosed", nNotClosed);
    }

    return true;
}
}
