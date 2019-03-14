// wiredtiger_record_store.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

#include <wiredtiger.h>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_stones.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

//#define RS_ITERATOR_TRACE(x) log() << "WTRS::Iterator " << x
#define RS_ITERATOR_TRACE(x)

namespace mongo {

using std::unique_ptr;
using std::string;

namespace {

static const int kMinimumRecordStoreVersion = 1;
static const int kCurrentRecordStoreVersion = 1;  // New record stores use this by default.
static const int kMaximumRecordStoreVersion = 1;
static_assert(kCurrentRecordStoreVersion >= kMinimumRecordStoreVersion,
              "kCurrentRecordStoreVersion >= kMinimumRecordStoreVersion");
static_assert(kCurrentRecordStoreVersion <= kMaximumRecordStoreVersion,
              "kCurrentRecordStoreVersion <= kMaximumRecordStoreVersion");

bool shouldUseOplogHack(OperationContext* opCtx, const std::string& uri) {
    StatusWith<BSONObj> appMetadata = WiredTigerUtil::getApplicationMetadata(opCtx, uri);
    if (!appMetadata.isOK()) {
        return false;
    }

    return (appMetadata.getValue().getIntField("oplogKeyExtractionVersion") == 1);
}

}  // namespace

MONGO_FP_DECLARE(WTWriteConflictException);
MONGO_FP_DECLARE(WTWriteConflictExceptionForReads);
MONGO_FP_DECLARE(WTPausePrimaryOplogDurabilityLoop);

const std::string kWiredTigerEngineName = "wiredTiger";

class WiredTigerRecordStore::OplogStones::InsertChange final : public RecoveryUnit::Change {
public:
    InsertChange(OplogStones* oplogStones,
                 int64_t bytesInserted,
                 RecordId highestInserted,
                 int64_t countInserted)
        : _oplogStones(oplogStones),
          _bytesInserted(bytesInserted),
          _highestInserted(highestInserted),
          _countInserted(countInserted) {}

    void commit() final {
        invariant(_bytesInserted >= 0);
        invariant(_highestInserted.isNormal());

        _oplogStones->_currentRecords.addAndFetch(_countInserted);
        int64_t newCurrentBytes = _oplogStones->_currentBytes.addAndFetch(_bytesInserted);
        if (newCurrentBytes >= _oplogStones->_minBytesPerStone) {
            _oplogStones->createNewStoneIfNeeded(_highestInserted);
        }
    }

    void rollback() final {}

private:
    OplogStones* _oplogStones;
    int64_t _bytesInserted;
    RecordId _highestInserted;
    int64_t _countInserted;
};

class WiredTigerRecordStore::OplogStones::TruncateChange final : public RecoveryUnit::Change {
public:
    TruncateChange(OplogStones* oplogStones) : _oplogStones(oplogStones) {}

    void commit() final {
        _oplogStones->_currentRecords.store(0);
        _oplogStones->_currentBytes.store(0);

        stdx::lock_guard<stdx::mutex> lk(_oplogStones->_mutex);
        _oplogStones->_stones.clear();
    }

    void rollback() final {}

private:
    OplogStones* _oplogStones;
};

WiredTigerRecordStore::OplogStones::OplogStones(OperationContext* txn, WiredTigerRecordStore* rs)
    : _rs(rs) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    invariant(rs->isCapped());
    invariant(rs->cappedMaxSize() > 0);
    unsigned long long maxSize = rs->cappedMaxSize();

    const unsigned long long kMinStonesToKeep = 10ULL;
    const unsigned long long kMaxStonesToKeep = 100ULL;

    unsigned long long numStones = maxSize / BSONObjMaxInternalSize;
    _numStonesToKeep = std::min(kMaxStonesToKeep, std::max(kMinStonesToKeep, numStones));
    _minBytesPerStone = maxSize / _numStonesToKeep;
    invariant(_minBytesPerStone > 0);

    _calculateStones(txn);
    _pokeReclaimThreadIfNeeded();  // Reclaim stones if over the limit.
}

bool WiredTigerRecordStore::OplogStones::isDead() {
    stdx::lock_guard<stdx::mutex> lk(_oplogReclaimMutex);
    return _isDead;
}

void WiredTigerRecordStore::OplogStones::kill() {
    {
        stdx::lock_guard<stdx::mutex> lk(_oplogReclaimMutex);
        _isDead = true;
    }
    _oplogReclaimCv.notify_one();
}

void WiredTigerRecordStore::OplogStones::awaitHasExcessStonesOrDead() {
    // Wait until kill() is called or there are too many oplog stones.
    stdx::unique_lock<stdx::mutex> lock(_oplogReclaimMutex);
    while (!_isDead && !hasExcessStones()) {
        _oplogReclaimCv.wait(lock);
    }
}

boost::optional<WiredTigerRecordStore::OplogStones::Stone>
WiredTigerRecordStore::OplogStones::peekOldestStoneIfNeeded() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (!hasExcessStones()) {
        return {};
    }

    return _stones.front();
}

void WiredTigerRecordStore::OplogStones::popOldestStone() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stones.pop_front();
}

void WiredTigerRecordStore::OplogStones::createNewStoneIfNeeded(RecordId lastRecord) {
    stdx::unique_lock<stdx::mutex> lk(_mutex, stdx::try_to_lock);
    if (!lk) {
        // Someone else is either already creating a new stone or popping the oldest one. In the
        // latter case, we let the next insert trigger the new stone's creation.
        return;
    }

    if (_currentBytes.load() < _minBytesPerStone) {
        // Must have raced to create a new stone, someone else already triggered it.
        return;
    }

    if (!_stones.empty() && lastRecord < _stones.back().lastRecord) {
        // Skip creating a new stone when the record's position comes before the most recently
        // created stone. We likely raced with another batch of inserts that caused us to try and
        // make multiples stones.
        return;
    }

    OplogStones::Stone stone = {_currentRecords.swap(0), _currentBytes.swap(0), lastRecord};
    _stones.push_back(stone);

    _pokeReclaimThreadIfNeeded();
}

void WiredTigerRecordStore::OplogStones::updateCurrentStoneAfterInsertOnCommit(
    OperationContext* txn, int64_t bytesInserted, RecordId highestInserted, int64_t countInserted) {
    txn->recoveryUnit()->registerChange(
        new InsertChange(this, bytesInserted, highestInserted, countInserted));
}

void WiredTigerRecordStore::OplogStones::clearStonesOnCommit(OperationContext* txn) {
    txn->recoveryUnit()->registerChange(new TruncateChange(this));
}

void WiredTigerRecordStore::OplogStones::updateStonesAfterCappedTruncateAfter(
    int64_t recordsRemoved, int64_t bytesRemoved, RecordId firstRemovedId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    int64_t numStonesToRemove = 0;
    int64_t recordsInStonesToRemove = 0;
    int64_t bytesInStonesToRemove = 0;

    // Compute the number and associated sizes of the records from stones that are either fully or
    // partially truncated.
    for (auto it = _stones.rbegin(); it != _stones.rend(); ++it) {
        if (it->lastRecord < firstRemovedId) {
            break;
        }
        numStonesToRemove++;
        recordsInStonesToRemove += it->records;
        bytesInStonesToRemove += it->bytes;
    }

    // Remove the stones corresponding to the records that were deleted.
    int64_t offset = _stones.size() - numStonesToRemove;
    _stones.erase(_stones.begin() + offset, _stones.end());

    // Account for any remaining records from a partially truncated stone in the stone currently
    // being filled.
    _currentRecords.addAndFetch(recordsInStonesToRemove - recordsRemoved);
    _currentBytes.addAndFetch(bytesInStonesToRemove - bytesRemoved);
}

void WiredTigerRecordStore::OplogStones::setMinBytesPerStone(int64_t size) {
    invariant(size > 0);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Only allow changing the minimum bytes per stone if no data has been inserted.
    invariant(_stones.size() == 0 && _currentRecords.load() == 0);
    _minBytesPerStone = size;
}

void WiredTigerRecordStore::OplogStones::setNumStonesToKeep(size_t numStones) {
    invariant(numStones > 0);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Only allow changing the number of stones to keep if no data has been inserted.
    invariant(_stones.size() == 0 && _currentRecords.load() == 0);
    _numStonesToKeep = numStones;
}

void WiredTigerRecordStore::OplogStones::_calculateStones(OperationContext* txn) {
    long long numRecords = _rs->numRecords(txn);
    long long dataSize = _rs->dataSize(txn);

    log() << "The size storer reports that the oplog contains " << numRecords
          << " records totaling to " << dataSize << " bytes";

    // Only use sampling to estimate where to place the oplog stones if the number of samples drawn
    // is less than 5% of the collection.
    const uint64_t kMinSampleRatioForRandCursor = 20;

    // If the oplog doesn't contain enough records to make sampling more efficient, then scan the
    // oplog to determine where to put down stones.
    if (numRecords <= 0 || dataSize <= 0 ||
        uint64_t(numRecords) <
            kMinSampleRatioForRandCursor * kRandomSamplesPerStone * _numStonesToKeep) {
        _calculateStonesByScanning(txn);
        return;
    }

    // Use the oplog's average record size to estimate the number of records in each stone, and thus
    // estimate the combined size of the records.
    double avgRecordSize = double(dataSize) / double(numRecords);
    double estRecordsPerStone = std::ceil(_minBytesPerStone / avgRecordSize);
    double estBytesPerStone = estRecordsPerStone * avgRecordSize;

    _calculateStonesBySampling(txn, int64_t(estRecordsPerStone), int64_t(estBytesPerStone));
}

void WiredTigerRecordStore::OplogStones::_calculateStonesByScanning(OperationContext* txn) {
    log() << "Scanning the oplog to determine where to place markers for truncation";

    long long numRecords = 0;
    long long dataSize = 0;

    auto cursor = _rs->getCursor(txn, true);
    while (auto record = cursor->next()) {
        _currentRecords.addAndFetch(1);
        int64_t newCurrentBytes = _currentBytes.addAndFetch(record->data.size());
        if (newCurrentBytes >= _minBytesPerStone) {
            LOG(1) << "Placing a marker at optime "
                   << Timestamp(record->id.repr()).toStringPretty();

            OplogStones::Stone stone = {_currentRecords.swap(0), _currentBytes.swap(0), record->id};
            _stones.push_back(stone);
        }

        numRecords++;
        dataSize += record->data.size();
    }

    _rs->updateStatsAfterRepair(txn, numRecords, dataSize);
}

void WiredTigerRecordStore::OplogStones::_calculateStonesBySampling(OperationContext* txn,
                                                                    int64_t estRecordsPerStone,
                                                                    int64_t estBytesPerStone) {
    Timestamp earliestOpTime;
    Timestamp latestOpTime;

    {
        const bool forward = true;
        auto cursor = _rs->getCursor(txn, forward);
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to determine the earliest optime, falling back to scanning the oplog";
            _calculateStonesByScanning(txn);
            return;
        }
        earliestOpTime = Timestamp(record->id.repr());
    }

    {
        const bool forward = false;
        auto cursor = _rs->getCursor(txn, forward);
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to determine the latest optime, falling back to scanning the oplog";
            _calculateStonesByScanning(txn);
            return;
        }
        latestOpTime = Timestamp(record->id.repr());
    }

    log() << "Sampling from the oplog between " << earliestOpTime.toStringPretty() << " and "
          << latestOpTime.toStringPretty() << " to determine where to place markers for truncation";

    int64_t wholeStones = _rs->numRecords(txn) / estRecordsPerStone;
    int64_t numSamples = kRandomSamplesPerStone * _rs->numRecords(txn) / estRecordsPerStone;

    log() << "Taking " << numSamples << " samples and assuming that each section of oplog contains"
          << " approximately " << estRecordsPerStone << " records totaling to " << estBytesPerStone
          << " bytes";

    // Inform the random cursor of the number of samples we intend to take. This allows it to
    // account for skew in the tree shape.
    const std::string extraConfig = str::stream() << "next_random_sample_size=" << numSamples;

    // Divide the oplog into 'wholeStones' logical sections, with each section containing
    // approximately 'estRecordsPerStone'. Do so by oversampling the oplog, sorting the samples in
    // order of their RecordId, and then choosing the samples expected to be near the right edge of
    // each logical section.
    auto cursor = _rs->getRandomCursorWithOptions(txn, extraConfig);
    std::vector<RecordId> oplogEstimates;
    for (int i = 0; i < numSamples; ++i) {
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to get enough random samples, falling back to scanning the oplog";
            _calculateStonesByScanning(txn);
            return;
        }
        oplogEstimates.push_back(record->id);
    }
    std::sort(oplogEstimates.begin(), oplogEstimates.end());

    for (int i = 1; i <= wholeStones; ++i) {
        // Use every (kRandomSamplesPerStone)th sample, starting with the
        // (kRandomSamplesPerStone - 1)th, as the last record for each stone.
        int sampleIndex = kRandomSamplesPerStone * i - 1;
        RecordId lastRecord = oplogEstimates[sampleIndex];

        log() << "Placing a marker at optime " << Timestamp(lastRecord.repr()).toStringPretty();
        OplogStones::Stone stone = {estRecordsPerStone, estBytesPerStone, lastRecord};
        _stones.push_back(stone);
    }

    // Account for the partially filled chunk.
    _currentRecords.store(_rs->numRecords(txn) - estRecordsPerStone * wholeStones);
    _currentBytes.store(_rs->dataSize(txn) - estBytesPerStone * wholeStones);
}

void WiredTigerRecordStore::OplogStones::_pokeReclaimThreadIfNeeded() {
    if (hasExcessStones()) {
        _oplogReclaimCv.notify_one();
    }
}

class WiredTigerRecordStore::Cursor final : public SeekableRecordCursor {
public:
    Cursor(OperationContext* txn, const WiredTigerRecordStore& rs, bool forward = true)
        : _rs(rs),
          _txn(txn),
          _forward(forward),
          _readUntilForOplog(WiredTigerRecoveryUnit::get(txn)->getOplogReadTill()) {
        _cursor.emplace(rs.getURI(), rs.tableId(), true, txn);
    }

    boost::optional<Record> next() final {
        if (_eof)
            return {};

        WT_CURSOR* c = _cursor->get();

        if (!_skipNextAdvance) {
            // Nothing after the next line can throw WCEs.
            // Note that an unpositioned (or eof) WT_CURSOR returns the first/last entry in the
            // table when you call next/prev.
            int advanceRet = WT_READ_CHECK(_forward ? c->next(c) : c->prev(c));
            if (advanceRet == WT_NOTFOUND) {
                _eof = true;
                return {};
            }
            invariantWTOK(advanceRet);
        }

        _skipNextAdvance = false;
        int64_t key;
        invariantWTOK(c->get_key(c, &key));
        RecordId id = _fromKey(key);

        if (_forward && _lastReturnedId >= id) {
            log() << "WTCursor::next -- c->next_key ( " << id
                  << ") was not greater than _lastReturnedId (" << _lastReturnedId
                  << ") which is a bug.";
            // Force a retry of the operation from our last known position by acting as-if
            // we received a WT_ROLLBACK error.
            throw WriteConflictException();
        }

        if (!isVisible(id)) {
            _eof = true;
            return {};
        }

        WT_ITEM value;
        invariantWTOK(c->get_value(c, &value));

        _lastReturnedId = id;
        return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
    }

    boost::optional<Record> seekExact(const RecordId& id) final {
        _skipNextAdvance = false;
        WT_CURSOR* c = _cursor->get();
        c->set_key(c, _makeKey(id));
        // Nothing after the next line can throw WCEs.
        int seekRet = WT_READ_CHECK(c->search(c));
        if (seekRet == WT_NOTFOUND) {
            _eof = true;
            return {};
        }
        invariantWTOK(seekRet);

        WT_ITEM value;
        invariantWTOK(c->get_value(c, &value));

        _lastReturnedId = id;
        _eof = false;
        return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
    }

    void save() final {
        try {
            if (_cursor)
                _cursor->reset();
        } catch (const WriteConflictException& wce) {
            // Ignore since this is only called when we are about to kill our transaction
            // anyway.
        }
    }

    void saveUnpositioned() final {
        save();
        _lastReturnedId = RecordId();
    }

    bool restore() final {
        if (!_cursor)
            _cursor.emplace(_rs.getURI(), _rs.tableId(), true, _txn);

        // This will ensure an active session exists, so any restored cursors will bind to it
        invariant(WiredTigerRecoveryUnit::get(_txn)->getSession(_txn) == _cursor->getSession());
        _skipNextAdvance = false;

        // If we've hit EOF, then this iterator is done and need not be restored.
        if (_eof)
            return true;

        if (_lastReturnedId.isNull())
            return true;

        WT_CURSOR* c = _cursor->get();
        c->set_key(c, _makeKey(_lastReturnedId));

        int cmp;
        int ret = WT_READ_CHECK(c->search_near(c, &cmp));
        if (ret == WT_NOTFOUND) {
            _eof = true;
            return !_rs._isCapped;
        }
        invariantWTOK(ret);

        if (cmp == 0)
            return true;  // Landed right where we left off.

        if (_rs._isCapped) {
            // Doc was deleted either by cappedDeleteAsNeeded() or cappedTruncateAfter().
            // It is important that we error out in this case so that consumers don't
            // silently get 'holes' when scanning capped collections. We don't make
            // this guarantee for normal collections so it is ok to skip ahead in that case.
            _eof = true;
            return false;
        }

        if (_forward && cmp > 0) {
            // We landed after where we were. Return our new location on the next call to next().
            _skipNextAdvance = true;
        } else if (!_forward && cmp < 0) {
            _skipNextAdvance = true;
        }

        return true;
    }

    void detachFromOperationContext() final {
        _txn = nullptr;
        _cursor = boost::none;
    }

    void reattachToOperationContext(OperationContext* txn) final {
        _txn = txn;
        // _cursor recreated in restore() to avoid risk of WT_ROLLBACK issues.
    }

private:
    bool isVisible(const RecordId& id) {
        if (!_rs._isCapped)
            return true;

        if (!_forward)
            return true;

        if (_readUntilForOplog.isNull() || !_rs._isOplog) {
            // this is the normal capped case
            return !_rs.isCappedHidden(id);
        }

        // this is for oplogs
        if (id == _readUntilForOplog) {
            // we allow if its been committed already
            return !_rs.isCappedHidden(id);
        }

        return id < _readUntilForOplog;
    }

    const WiredTigerRecordStore& _rs;
    OperationContext* _txn;
    const bool _forward;
    bool _skipNextAdvance = false;
    boost::optional<WiredTigerCursor> _cursor;
    bool _eof = false;
    RecordId _lastReturnedId;  // If null, need to seek to first/last record.
    const RecordId _readUntilForOplog;
};

StatusWith<std::string> WiredTigerRecordStore::parseOptionsField(const BSONObj options) {
    StringBuilder ss;
    BSONForEach(elem, options) {
        if (elem.fieldNameStringData() == "configString") {
            Status status = WiredTigerUtil::checkTableCreationOptions(elem);
            if (!status.isOK()) {
                return status;
            }
            ss << elem.valueStringData() << ',';
        } else {
            // Return error on first unrecognized field.
            return StatusWith<std::string>(ErrorCodes::InvalidOptions,
                                           str::stream() << '\'' << elem.fieldNameStringData()
                                                         << '\'' << " is not a supported option.");
        }
    }
    return StatusWith<std::string>(ss.str());
}

class WiredTigerRecordStore::RandomCursor final : public RecordCursor {
public:
    RandomCursor(OperationContext* txn, const WiredTigerRecordStore& rs, StringData config)
        : _cursor(nullptr), _rs(&rs), _txn(txn), _config(config.toString() + ",next_random") {
        restore();
    }

    ~RandomCursor() {
        if (_cursor)
            detachFromOperationContext();
    }

    boost::optional<Record> next() final {
        int advanceRet = WT_READ_CHECK(_cursor->next(_cursor));
        if (advanceRet == WT_NOTFOUND)
            return {};
        invariantWTOK(advanceRet);

        int64_t key;
        invariantWTOK(_cursor->get_key(_cursor, &key));
        const RecordId id = _fromKey(key);

        WT_ITEM value;
        invariantWTOK(_cursor->get_value(_cursor, &value));

        return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
    }

    void save() final {
        if (_cursor && !wt_keeptxnopen()) {
            try {
                _cursor->reset(_cursor);
            } catch (const WriteConflictException& wce) {
                // Ignore since this is only called when we are about to kill our transaction
                // anyway.
            }
        }
    }

    bool restore() final {
        // We can't use the CursorCache since this cursor needs a special config string.
        WT_SESSION* session = WiredTigerRecoveryUnit::get(_txn)->getSession(_txn)->getSession();

        if (!_cursor) {
            invariantWTOK(session->open_cursor(
                session, _rs->_uri.c_str(), nullptr, _config.c_str(), &_cursor));
            invariant(_cursor);
        }
        return true;
    }
    void detachFromOperationContext() final {
        invariant(_txn);
        _txn = nullptr;
        if (_cursor) {
            invariantWTOK(_cursor->close(_cursor));
        }
        _cursor = nullptr;
    }
    void reattachToOperationContext(OperationContext* txn) final {
        invariant(!_txn);
        _txn = txn;
    }

private:
    WT_CURSOR* _cursor;
    const WiredTigerRecordStore* _rs;
    OperationContext* _txn;
    const std::string _config;
};


// static
StatusWith<std::string> WiredTigerRecordStore::generateCreateString(
    const std::string& engineName,
    StringData ns,
    const CollectionOptions& options,
    StringData extraStrings) {
    // Separate out a prefix and suffix in the default string. User configuration will
    // override values in the prefix, but not values in the suffix.
    str::stream ss;
    ss << "type=file,";
    // Setting this larger than 10m can hurt latencies and throughput degradation if this
    // is the oplog.  See SERVER-16247
    ss << "memory_page_max=10m,";
    // Choose a higher split percent, since most usage is append only. Allow some space
    // for workloads where updates increase the size of documents.
    ss << "split_pct=90,";
    ss << "leaf_value_max=64MB,";
    ss << "checksum=on,";
    if (wiredTigerGlobalOptions.useCollectionPrefixCompression) {
        ss << "prefix_compression,";
    }

    ss << "block_compressor=" << wiredTigerGlobalOptions.collectionBlockCompressor << ",";

    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())->getOpenConfig(ns);

    ss << extraStrings << ",";

    StatusWith<std::string> customOptions =
        parseOptionsField(options.storageEngine.getObjectField(engineName));
    if (!customOptions.isOK())
        return customOptions;

    ss << customOptions.getValue();

    if (NamespaceString::oplog(ns)) {
        // force file for oplog
        ss << "type=file,";
        // Tune down to 10m.  See SERVER-16247
        ss << "memory_page_max=10m,";
    }

    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.

    ss << "key_format=q,value_format=u";

    // Record store metadata
    ss << ",app_metadata=(formatVersion=" << kCurrentRecordStoreVersion;
    if (NamespaceString::oplog(ns)) {
        ss << ",oplogKeyExtractionVersion=1";
    }
    ss << ")";

    return StatusWith<std::string>(ss);
}

WiredTigerRecordStore::WiredTigerRecordStore(OperationContext* ctx,
                                             StringData ns,
                                             StringData uri,
                                             std::string engineName,
                                             bool isCapped,
                                             bool isEphemeral,
                                             int64_t cappedMaxSize,
                                             int64_t cappedMaxDocs,
                                             CappedCallback* cappedCallback,
                                             WiredTigerSizeStorer* sizeStorer)
    : RecordStore(ns),
      _uri(uri.toString()),
      _tableId(WiredTigerSession::genTableId()),
      _engineName(engineName),
      _isCapped(isCapped),
      _isEphemeral(isEphemeral),
      _isOplog(NamespaceString::oplog(ns)),
      _cappedMaxSize(cappedMaxSize),
      _cappedMaxSizeSlack(std::min(cappedMaxSize / 10, int64_t(16 * 1024 * 1024))),
      _cappedMaxDocs(cappedMaxDocs),
      _cappedSleep(0),
      _cappedSleepMS(0),
      _cappedCallback(cappedCallback),
      _cappedDeleteCheckCount(0),
      _useOplogHack(shouldUseOplogHack(ctx, _uri)),
      _sizeStorer(sizeStorer),
      _sizeStorerCounter(0),
      _shuttingDown(false) {
    Status versionStatus = WiredTigerUtil::checkApplicationMetadataFormatVersion(
        ctx, uri, kMinimumRecordStoreVersion, kMaximumRecordStoreVersion);
    if (!versionStatus.isOK()) {
        if (versionStatus.code() == ErrorCodes::FailedToParse) {
            uasserted(28548, versionStatus.reason());
        } else {
            fassertFailedNoTrace(34433);
        }
    }

    if (_isCapped) {
        invariant(_cappedMaxSize > 0);
        invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
    } else {
        invariant(_cappedMaxSize == -1);
        invariant(_cappedMaxDocs == -1);
    }

    // Find the largest RecordId currently in use and estimate the number of records.
    Cursor cursor(ctx, *this, /*forward=*/false);
    if (auto record = cursor.next()) {
        int64_t max = _makeKey(record->id);
        _oplog_highestSeen = record->id;
        _nextIdNum.store(1 + max);

        if (_sizeStorer) {
            long long numRecords;
            long long dataSize;
            _sizeStorer->loadFromCache(uri, &numRecords, &dataSize);
            _numRecords.store(numRecords);
            _dataSize.store(dataSize);
            _sizeStorer->onCreate(this, numRecords, dataSize);
        } else {
            LOG(1) << "Doing scan of collection " << ns << " to get size and count info";

            _numRecords.store(0);
            _dataSize.store(0);

            do {
                _numRecords.fetchAndAdd(1);
                _dataSize.fetchAndAdd(record->data.size());
            } while ((record = cursor.next()));
        }
    } else {
        _dataSize.store(0);
        _numRecords.store(0);
        // Need to start at 1 so we are always higher than RecordId::min()
        _nextIdNum.store(1);
        if (sizeStorer)
            _sizeStorer->onCreate(this, 0, 0);
    }

    if (WiredTigerKVEngine::initRsOplogBackgroundThread(ns)) {
        _oplogStones = std::make_shared<OplogStones>(ctx, this);
    }

    if (_isOplog) {
        _oplogJournalThread = stdx::thread(&WiredTigerRecordStore::_oplogJournalThreadLoop,
                                           this,
                                           WiredTigerRecoveryUnit::get(ctx)->getSessionCache());
    }
}

WiredTigerRecordStore::~WiredTigerRecordStore() {
    {
        stdx::lock_guard<boost::timed_mutex> lk(_cappedDeleterMutex);  // NOLINT
        stdx::lock_guard<stdx::mutex> lk2(_uncommittedRecordIdsMutex);
        _shuttingDown = true;
    }

    LOG(1) << "~WiredTigerRecordStore for: " << ns();
    if (_sizeStorer) {
        _sizeStorer->onDestroy(this);
    }

    if (_oplogStones) {
        _oplogStones->kill();
    }

    if (_oplogJournalThread.joinable()) {
        _opsWaitingForJournalCV.notify_one();
        _oplogJournalThread.join();
    }
}

const char* WiredTigerRecordStore::name() const {
    return _engineName.c_str();
}

bool WiredTigerRecordStore::inShutdown() const {
    stdx::lock_guard<boost::timed_mutex> lk(_cappedDeleterMutex);  // NOLINT
    return _shuttingDown;
}

long long WiredTigerRecordStore::dataSize(OperationContext* txn) const {
    return _dataSize.load();
}

long long WiredTigerRecordStore::numRecords(OperationContext* txn) const {
    return _numRecords.load();
}

bool WiredTigerRecordStore::isCapped() const {
    return _isCapped;
}

int64_t WiredTigerRecordStore::cappedMaxDocs() const {
    invariant(_isCapped);
    return _cappedMaxDocs;
}

int64_t WiredTigerRecordStore::cappedMaxSize() const {
    invariant(_isCapped);
    return _cappedMaxSize;
}

int64_t WiredTigerRecordStore::storageSize(OperationContext* txn,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel) const {
    if (_isEphemeral) {
        return dataSize(txn);
    }
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn);
    StatusWith<int64_t> result =
        WiredTigerUtil::getStatisticsValueAs<int64_t>(session->getSession(),
                                                      "statistics:" + getURI(),
                                                      "statistics=(size)",
                                                      WT_STAT_DSRC_BLOCK_SIZE);
    uassertStatusOK(result.getStatus());

    int64_t size = result.getValue();

    if (size == 0 && _isCapped) {
        // Many things assume an empty capped collection still takes up space.
        return 1;
    }
    return size;
}

// Retrieve the value from a positioned cursor.
RecordData WiredTigerRecordStore::_getData(const WiredTigerCursor& cursor) const {
    WT_ITEM value;
    int ret = cursor->get_value(cursor.get(), &value);
    invariantWTOK(ret);

    SharedBuffer data = SharedBuffer::allocate(value.size);
    memcpy(data.get(), value.data, value.size);
    return RecordData(data, value.size);
}

RecordData WiredTigerRecordStore::dataFor(OperationContext* txn, const RecordId& id) const {
    // ownership passes to the shared_array created below
    WiredTigerCursor curwrap(_uri, _tableId, true, txn);
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    c->set_key(c, _makeKey(id));
    int ret = WT_READ_CHECK(c->search(c));
    massert(28556, "Didn't find RecordId in WiredTigerRecordStore", ret != WT_NOTFOUND);
    invariantWTOK(ret);
    return _getData(curwrap);
}

bool WiredTigerRecordStore::findRecord(OperationContext* txn,
                                       const RecordId& id,
                                       RecordData* out) const {
    WiredTigerCursor curwrap(_uri, _tableId, true, txn);
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    c->set_key(c, _makeKey(id));
    int ret = WT_READ_CHECK(c->search(c));
    if (ret == WT_NOTFOUND) {
        return false;
    }
    invariantWTOK(ret);
    *out = _getData(curwrap);
    return true;
}

void WiredTigerRecordStore::deleteRecord(OperationContext* txn, const RecordId& id) {
    // Deletes should never occur on a capped collection because truncation uses
    // WT_SESSION::truncate().
    invariant(!isCapped());

    WiredTigerCursor cursor(_uri, _tableId, true, txn);
    cursor.assertInActiveTxn();
    WT_CURSOR* c = cursor.get();
    c->set_key(c, _makeKey(id));
    int ret = WT_READ_CHECK(c->search(c));
    invariantWTOK(ret);

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret);

    int64_t old_length = old_value.size;

    ret = WT_OP_CHECK(c->remove(c));
    invariantWTOK(ret);

    _changeNumRecords(txn, -1);
    _increaseDataSize(txn, -old_length);
}

bool WiredTigerRecordStore::cappedAndNeedDelete() const {
    if (!_isCapped)
        return false;

    if (_dataSize.load() >= _cappedMaxSize)
        return true;

    if ((_cappedMaxDocs != -1) && (_numRecords.load() > _cappedMaxDocs))
        return true;

    return false;
}

int64_t WiredTigerRecordStore::cappedDeleteAsNeeded(OperationContext* txn,
                                                    const RecordId& justInserted) {
    invariant(!_oplogStones);

    // We only want to do the checks occasionally as they are expensive.
    // This variable isn't thread safe, but has loose semantics anyway.
    dassert(!_isOplog || _cappedMaxDocs == -1);

    if (!cappedAndNeedDelete())
        return 0;

    // ensure only one thread at a time can do deletes, otherwise they'll conflict.
    boost::unique_lock<boost::timed_mutex> lock(_cappedDeleterMutex, boost::defer_lock);  // NOLINT

    if (_cappedMaxDocs != -1) {
        lock.lock();  // Max docs has to be exact, so have to check every time.
    } else {
        if (!lock.try_lock()) {
            // Someone else is deleting old records. Apply back-pressure if too far behind,
            // otherwise continue.
            if ((_dataSize.load() - _cappedMaxSize) < _cappedMaxSizeSlack)
                return 0;

            // Don't wait forever: we're in a transaction, we could block eviction.
            Date_t before = Date_t::now();
            bool gotLock = lock.try_lock_for(boost::chrono::milliseconds(200));  // NOLINT
            auto delay = boost::chrono::milliseconds(                            // NOLINT
                durationCount<Milliseconds>(Date_t::now() - before));
            _cappedSleep.fetchAndAdd(1);
            _cappedSleepMS.fetchAndAdd(delay.count());
            if (!gotLock)
                return 0;

            // If we already waited, let someone else do cleanup unless we are significantly
            // over the limit.
            if ((_dataSize.load() - _cappedMaxSize) < (2 * _cappedMaxSizeSlack))
                return 0;
        }
    }

    return cappedDeleteAsNeeded_inlock(txn, justInserted);
}

int64_t WiredTigerRecordStore::cappedDeleteAsNeeded_inlock(OperationContext* txn,
                                                           const RecordId& justInserted) {
    // we do this in a side transaction in case it aborts
    WiredTigerRecoveryUnit* realRecoveryUnit =
        checked_cast<WiredTigerRecoveryUnit*>(txn->releaseRecoveryUnit());
    invariant(realRecoveryUnit);
    WiredTigerSessionCache* sc = realRecoveryUnit->getSessionCache();
    OperationContext::RecoveryUnitState const realRUstate =
        txn->setRecoveryUnit(new WiredTigerRecoveryUnit(sc), OperationContext::kNotInUnitOfWork);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn)->getSession();

    int64_t dataSize = _dataSize.load();
    int64_t numRecords = _numRecords.load();

    int64_t sizeOverCap = (dataSize > _cappedMaxSize) ? dataSize - _cappedMaxSize : 0;
    int64_t sizeSaved = 0;
    int64_t docsOverCap = 0, docsRemoved = 0;
    if (_cappedMaxDocs != -1 && numRecords > _cappedMaxDocs)
        docsOverCap = numRecords - _cappedMaxDocs;

    try {
        WriteUnitOfWork wuow(txn);

        WiredTigerCursor curwrap(_uri, _tableId, true, txn);
        WT_CURSOR* truncateEnd = curwrap.get();
        RecordId newestIdToDelete;
        int ret = 0;
        bool positioned = false;  // Mark if the cursor is on the first key
        int64_t savedFirstKey = 0;

        // If we know where the first record is, go to it
        if (_cappedFirstRecord != RecordId()) {
            int64_t key = _makeKey(_cappedFirstRecord);
            truncateEnd->set_key(truncateEnd, key);
            ret = WT_READ_CHECK(truncateEnd->search(truncateEnd));
            if (ret == 0) {
                positioned = true;
                savedFirstKey = key;
            }
        }

        // Advance the cursor truncateEnd until we find a suitable end point for our truncate
        while ((sizeSaved < sizeOverCap || docsRemoved < docsOverCap) && (docsRemoved < 20000) &&
               (positioned || (ret = WT_READ_CHECK(truncateEnd->next(truncateEnd))) == 0)) {
            positioned = false;
            int64_t key;
            invariantWTOK(truncateEnd->get_key(truncateEnd, &key));

            // don't go past the record we just inserted
            newestIdToDelete = _fromKey(key);
            if (newestIdToDelete >= justInserted)  // TODO: use oldest uncommitted instead
                break;

            if (_shuttingDown)
                break;

            WT_ITEM old_value;
            invariantWTOK(truncateEnd->get_value(truncateEnd, &old_value));

            ++docsRemoved;
            sizeSaved += old_value.size;

            stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
            if (_cappedCallback) {
                uassertStatusOK(_cappedCallback->aboutToDeleteCapped(
                    txn,
                    newestIdToDelete,
                    RecordData(static_cast<const char*>(old_value.data), old_value.size)));
            }
        }

        if (ret != WT_NOTFOUND) {
            invariantWTOK(ret);
        }

        if (docsRemoved > 0) {
            // if we scanned to the end of the collection or past our insert, go back one
            if (ret == WT_NOTFOUND || newestIdToDelete >= justInserted) {
                ret = WT_READ_CHECK(truncateEnd->prev(truncateEnd));
            }
            invariantWTOK(ret);

            RecordId firstRemainingId;
            ret = truncateEnd->next(truncateEnd);
            if (ret != WT_NOTFOUND) {
                invariantWTOK(ret);
                int64_t key;
                invariantWTOK(truncateEnd->get_key(truncateEnd, &key));
                firstRemainingId = _fromKey(key);
            }
            invariantWTOK(truncateEnd->prev(truncateEnd));  // put the cursor back where it was

            WiredTigerCursor startWrap(_uri, _tableId, true, txn);
            WT_CURSOR* truncateStart = startWrap.get();

            // If we know where the start point is, set it for the truncate
            if (savedFirstKey != 0) {
                truncateStart->set_key(truncateStart, savedFirstKey);
            } else {
                truncateStart = NULL;
            }
            ret = session->truncate(session, NULL, truncateStart, truncateEnd, NULL);

            if (ret == ENOENT || ret == WT_NOTFOUND) {
                // TODO we should remove this case once SERVER-17141 is resolved
                log() << "Soft failure truncating capped collection. Will try again later.";
                docsRemoved = 0;
            } else {
                invariantWTOK(ret);
                _changeNumRecords(txn, -docsRemoved);
                _increaseDataSize(txn, -sizeSaved);
                wuow.commit();
                // Save the key for the next round
                _cappedFirstRecord = firstRemainingId;
            }
        }
    } catch (const WriteConflictException& wce) {
        delete txn->releaseRecoveryUnit();
        txn->setRecoveryUnit(realRecoveryUnit, realRUstate);
        log() << "got conflict truncating capped, ignoring";
        return 0;
    } catch (...) {
        delete txn->releaseRecoveryUnit();
        txn->setRecoveryUnit(realRecoveryUnit, realRUstate);
        throw;
    }

    delete txn->releaseRecoveryUnit();
    txn->setRecoveryUnit(realRecoveryUnit, realRUstate);
    return docsRemoved;
}

bool WiredTigerRecordStore::yieldAndAwaitOplogDeletionRequest(OperationContext* txn) {
    // Create another reference to the oplog stones while holding a lock on the collection to
    // prevent it from being destructed.
    std::shared_ptr<OplogStones> oplogStones = _oplogStones;

    Locker* locker = txn->lockState();
    Locker::LockSnapshot snapshot;

    // Release any locks before waiting on the condition variable. It is illegal to access any
    // methods or members of this record store after this line because it could be deleted.
    bool releasedAnyLocks = locker->saveLockStateAndUnlock(&snapshot);
    invariant(releasedAnyLocks);

    // The top-level locks were freed, so also release any potential low-level (storage engine)
    // locks that might be held.
    txn->recoveryUnit()->abandonSnapshot();

    // Wait for an oplog deletion request, or for this record store to have been destroyed.
    oplogStones->awaitHasExcessStonesOrDead();

    // Reacquire the locks that were released.
    locker->restoreLockState(snapshot);

    return !oplogStones->isDead();
}

void WiredTigerRecordStore::reclaimOplog(OperationContext* txn) {
    while (auto stone = _oplogStones->peekOldestStoneIfNeeded()) {
        invariant(stone->lastRecord.isNormal());

        LOG(1) << "Truncating the oplog between " << _oplogStones->firstRecord << " and "
               << stone->lastRecord << " to remove approximately " << stone->records
               << " records totaling to " << stone->bytes << " bytes";

        WiredTigerRecoveryUnit* ru = WiredTigerRecoveryUnit::get(txn);
        WT_SESSION* session = ru->getSession(txn)->getSession();

        try {
            WriteUnitOfWork wuow(txn);

            WiredTigerCursor startwrap(_uri, _tableId, true, txn);
            WT_CURSOR* start = startwrap.get();
            start->set_key(start, _makeKey(_oplogStones->firstRecord));

            WiredTigerCursor endwrap(_uri, _tableId, true, txn);
            WT_CURSOR* end = endwrap.get();
            end->set_key(end, _makeKey(stone->lastRecord));

            invariantWTOK(session->truncate(session, nullptr, start, end, nullptr));
            _changeNumRecords(txn, -stone->records);
            _increaseDataSize(txn, -stone->bytes);

            wuow.commit();

            // Remove the stone after a successful truncation.
            _oplogStones->popOldestStone();

            // Stash the truncate point for next time to cleanly skip over tombstones, etc.
            _oplogStones->firstRecord = stone->lastRecord;
        } catch (const WriteConflictException& wce) {
            LOG(1) << "Caught WriteConflictException while truncating oplog entries, retrying";
        }
    }

    LOG(1) << "Finished truncating the oplog, it now contains approximately " << _numRecords.load()
           << " records totaling to " << _dataSize.load() << " bytes";
}

Status WiredTigerRecordStore::insertRecords(OperationContext* txn,
                                            std::vector<Record>* records,
                                            bool enforceQuota) {
    // We are kind of cheating on capped collections since we write all of them at once ....
    // Simplest way out would be to just block vector writes for everything except oplog ?
    int totalLength = 0;
    for (auto& record : *records)
        totalLength += record.data.size();

    // caller will retry one element at a time
    if (_isCapped && totalLength > _cappedMaxSize)
        return Status(ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize");

    WiredTigerCursor curwrap(_uri, _tableId, true, txn);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    RecordId highestId = RecordId();
    dassert(!records->empty());
    for (auto& record : *records) {
        if (_useOplogHack) {
            StatusWith<RecordId> status =
                oploghack::extractKey(record.data.data(), record.data.size());
            if (!status.isOK())
                return status.getStatus();
            record.id = status.getValue();
        } else if (_isCapped) {
            stdx::lock_guard<stdx::mutex> lk(_uncommittedRecordIdsMutex);
            record.id = _nextId();
            _addUncommittedRecordId_inlock(txn, record.id);
        } else {
            record.id = _nextId();
        }
        dassert(record.id > highestId);
        highestId = record.id;
    }

    if (_useOplogHack && (highestId > _oplog_highestSeen)) {
        stdx::lock_guard<stdx::mutex> lk(_uncommittedRecordIdsMutex);
        if (highestId > _oplog_highestSeen)
            _oplog_highestSeen = highestId;
    }

    for (auto& record : *records) {
        c->set_key(c, _makeKey(record.id));
        WiredTigerItem value(record.data.data(), record.data.size());
        c->set_value(c, value.Get());
        int ret = WT_OP_CHECK(c->insert(c));
        if (ret)
            return wtRCToStatus(ret, "WiredTigerRecordStore::insertRecord");
    }

    _changeNumRecords(txn, records->size());
    _increaseDataSize(txn, totalLength);

    if (_oplogStones) {
        _oplogStones->updateCurrentStoneAfterInsertOnCommit(
            txn, totalLength, highestId, records->size());
    } else {
        cappedDeleteAsNeeded(txn, highestId);
    }

    return Status::OK();
}

StatusWith<RecordId> WiredTigerRecordStore::insertRecord(OperationContext* txn,
                                                         const char* data,
                                                         int len,
                                                         bool enforceQuota) {
    std::vector<Record> records;
    Record record = {RecordId(), RecordData(data, len)};
    records.push_back(record);
    Status status = insertRecords(txn, &records, enforceQuota);
    if (!status.isOK())
        return StatusWith<RecordId>(status);
    return StatusWith<RecordId>(records[0].id);
}

void WiredTigerRecordStore::_dealtWithCappedId(SortedRecordIds::iterator it, bool didCommit) {
    invariant(it->isNormal());
    stdx::lock_guard<stdx::mutex> lk(_uncommittedRecordIdsMutex);
    if (didCommit && _isOplog && *it != _oplog_highestSeen) {
        // Defer removal from _uncommittedRecordIds until it is durable. We don't need to wait for
        // durability of ops that didn't commit because they won't become durable.
        // As an optimization, we only defer visibility until durable if new ops were created while
        // we were pending. This makes single-threaded w>1 workloads faster and is safe because
        // durability follows commit order for commits that are fully sequenced (B doesn't call
        // commit until after A's commit call returns).
        const bool wasEmpty = _opsWaitingForJournal.empty();
        _opsWaitingForJournal.push_back(it);
        if (wasEmpty) {
            _opsWaitingForJournalCV.notify_one();
        }
    } else {
        _uncommittedRecordIds.erase(it);
        _opsBecameVisibleCV.notify_all();
    }
}

bool WiredTigerRecordStore::isCappedHidden(const RecordId& id) const {
    stdx::lock_guard<stdx::mutex> lk(_uncommittedRecordIdsMutex);
    if (_uncommittedRecordIds.empty()) {
        return false;
    }
    return _uncommittedRecordIds.front() <= id;
}

RecordId WiredTigerRecordStore::lowestCappedHiddenRecord() const {
    stdx::lock_guard<stdx::mutex> lk(_uncommittedRecordIdsMutex);
    return _uncommittedRecordIds.empty() ? RecordId() : _uncommittedRecordIds.front();
}

StatusWith<RecordId> WiredTigerRecordStore::insertRecord(OperationContext* txn,
                                                         const DocWriter* doc,
                                                         bool enforceQuota) {
    const int len = doc->documentSize();

    std::unique_ptr<char[]> buf(new char[len]);
    doc->writeDocument(buf.get());

    return insertRecord(txn, buf.get(), len, enforceQuota);
}

StatusWith<RecordId> WiredTigerRecordStore::updateRecord(OperationContext* txn,
                                                         const RecordId& id,
                                                         const char* data,
                                                         int len,
                                                         bool enforceQuota,
                                                         UpdateNotifier* notifier) {
    WiredTigerCursor curwrap(_uri, _tableId, true, txn);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    c->set_key(c, _makeKey(id));
    int ret = WT_READ_CHECK(c->search(c));
    invariantWTOK(ret);

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret);

    int64_t old_length = old_value.size;

    if (_oplogStones && len != old_length) {
        return {ErrorCodes::IllegalOperation, "Cannot change the size of a document in the oplog"};
    }

    c->set_key(c, _makeKey(id));
    WiredTigerItem value(data, len);
    c->set_value(c, value.Get());
    ret = WT_OP_CHECK(c->insert(c));
    invariantWTOK(ret);

    _increaseDataSize(txn, len - old_length);
    if (!_oplogStones) {
        cappedDeleteAsNeeded(txn, id);
    }

    return StatusWith<RecordId>(id);
}

bool WiredTigerRecordStore::updateWithDamagesSupported() const {
    return false;
}

StatusWith<RecordData> WiredTigerRecordStore::updateWithDamages(
    OperationContext* txn,
    const RecordId& id,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {
    MONGO_UNREACHABLE;
}

void WiredTigerRecordStore::_oplogSetStartHack(WiredTigerRecoveryUnit* wru) const {
    stdx::lock_guard<stdx::mutex> lk(_uncommittedRecordIdsMutex);
    if (_uncommittedRecordIds.empty()) {
        wru->setOplogReadTill(_oplog_highestSeen);
    } else {
        wru->setOplogReadTill(_uncommittedRecordIds.front());
    }
}

std::unique_ptr<SeekableRecordCursor> WiredTigerRecordStore::getCursor(OperationContext* txn,
                                                                       bool forward) const {
    if (_isOplog && forward) {
        WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(txn);
        // If we already have a snapshot we don't know what it can see, unless we know no one
        // else could be writing (because we hold an exclusive lock).
        if (wru->inActiveTxn() && !txn->lockState()->isNoop() &&
            !txn->lockState()->isCollectionLockedForMode(_ns, MODE_X)) {
            throw WriteConflictException();
        }
        _oplogSetStartHack(wru);
    }

    return stdx::make_unique<Cursor>(txn, *this, forward);
}

std::unique_ptr<RecordCursor> WiredTigerRecordStore::getRandomCursor(OperationContext* txn) const {
    const char* extraConfig = "";
    return getRandomCursorWithOptions(txn, extraConfig);
}

std::unique_ptr<RecordCursor> WiredTigerRecordStore::getRandomCursorWithOptions(
    OperationContext* txn, StringData extraConfig) const {
    return stdx::make_unique<RandomCursor>(txn, *this, extraConfig);
}

std::vector<std::unique_ptr<RecordCursor>> WiredTigerRecordStore::getManyCursors(
    OperationContext* txn) const {
    std::vector<std::unique_ptr<RecordCursor>> cursors(1);
    cursors[0] = stdx::make_unique<Cursor>(txn, *this, /*forward=*/true);
    return cursors;
}

Status WiredTigerRecordStore::truncate(OperationContext* txn) {
    WiredTigerCursor startWrap(_uri, _tableId, true, txn);
    WT_CURSOR* start = startWrap.get();
    int ret = WT_READ_CHECK(start->next(start));
    // Empty collections don't have anything to truncate.
    if (ret == WT_NOTFOUND) {
        return Status::OK();
    }
    invariantWTOK(ret);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn)->getSession();
    invariantWTOK(WT_OP_CHECK(session->truncate(session, NULL, start, NULL, NULL)));
    _changeNumRecords(txn, -numRecords(txn));
    _increaseDataSize(txn, -dataSize(txn));

    if (_oplogStones) {
        _oplogStones->clearStonesOnCommit(txn);
    }

    return Status::OK();
}

Status WiredTigerRecordStore::compact(OperationContext* txn,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats) {
    WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(txn)->getSessionCache();
    if (!cache->isEphemeral()) {
        WiredTigerSession* session = cache->getSession();
        WT_SESSION* s = session->getSession();
        int ret = s->compact(s, getURI().c_str(), "timeout=0");
        invariantWTOK(ret);
        cache->releaseSession(session);
    }
    return Status::OK();
}

Status WiredTigerRecordStore::validate(OperationContext* txn,
                                       bool full,
                                       bool scanData,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results,
                                       BSONObjBuilder* output) {
    if (!_isEphemeral) {
        int err = WiredTigerUtil::verifyTable(txn, _uri, &results->errors);
        if (err == EBUSY) {
            const char* msg = "verify() returned EBUSY. Not treating as invalid.";
            warning() << msg;
            results->errors.push_back(msg);
        } else if (err) {
            std::string msg = str::stream() << "verify() returned " << wiredtiger_strerror(err)
                                            << ". "
                                            << "This indicates structural damage. "
                                            << "Not examining individual documents.";
            error() << msg;
            results->errors.push_back(msg);
            results->valid = false;
            return Status::OK();
        }
    }

    long long nrecords = 0;
    long long dataSizeTotal = 0;
    results->valid = true;
    Cursor cursor(txn, *this, true);
    while (auto record = cursor.next()) {
        ++nrecords;
        auto dataSize = record->data.size();
        dataSizeTotal += dataSize;
        if (full && scanData) {
            size_t validatedSize;
            Status status = adaptor->validate(record->data, &validatedSize);

            // The validatedSize equals dataSize below is not a general requirement, but must be
            // true for WT today because we never pad records.
            if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
                results->valid = false;
                results->errors.push_back(str::stream() << record->id << " is corrupted");
            }
        }
    }

    if (_sizeStorer && results->valid) {
        if (nrecords != _numRecords.load() || dataSizeTotal != _dataSize.load()) {
            warning() << _uri << ": Existing record and data size counters (" << _numRecords.load()
                      << " records " << _dataSize.load() << " bytes) "
                      << "are inconsistent with validation results (" << nrecords << " records "
                      << dataSizeTotal << " bytes). "
                      << "Updating counters with new values.";
        }
        _numRecords.store(nrecords);
        _dataSize.store(dataSizeTotal);
        _sizeStorer->storeToCache(_uri, _numRecords.load(), _dataSize.load());
    }

    output->appendNumber("nrecords", nrecords);
    return Status::OK();
}

void WiredTigerRecordStore::appendCustomStats(OperationContext* txn,
                                              BSONObjBuilder* result,
                                              double scale) const {
    result->appendBool("capped", _isCapped);
    if (_isCapped) {
        result->appendIntOrLL("max", _cappedMaxDocs);
        result->appendIntOrLL("maxSize", static_cast<long long>(_cappedMaxSize / scale));
        result->appendIntOrLL("sleepCount", _cappedSleep.load());
        result->appendIntOrLL("sleepMS", _cappedSleepMS.load());
    }
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn);
    WT_SESSION* s = session->getSession();
    BSONObjBuilder bob(result->subobjStart(_engineName));
    {
        BSONObjBuilder metadata(bob.subobjStart("metadata"));
        Status status = WiredTigerUtil::getApplicationMetadata(txn, getURI(), &metadata);
        if (!status.isOK()) {
            metadata.append("error", "unable to retrieve metadata");
            metadata.append("code", static_cast<int>(status.code()));
            metadata.append("reason", status.reason());
        }
    }

    std::string type, sourceURI;
    WiredTigerUtil::fetchTypeAndSourceURI(txn, _uri, &type, &sourceURI);
    StatusWith<std::string> metadataResult = WiredTigerUtil::getMetadata(txn, sourceURI);
    StringData creationStringName("creationString");
    if (!metadataResult.isOK()) {
        BSONObjBuilder creationString(bob.subobjStart(creationStringName));
        creationString.append("error", "unable to retrieve creation config");
        creationString.append("code", static_cast<int>(metadataResult.getStatus().code()));
        creationString.append("reason", metadataResult.getStatus().reason());
    } else {
        bob.append("creationString", metadataResult.getValue());
        // Type can be "lsm" or "file"
        bob.append("type", type);
    }

    Status status =
        WiredTigerUtil::exportTableToBSON(s, "statistics:" + getURI(), "statistics=(fast)", &bob);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }
}

Status WiredTigerRecordStore::touch(OperationContext* txn, BSONObjBuilder* output) const {
    if (_isEphemeral) {
        // Everything is already in memory.
        return Status::OK();
    }
    return Status(ErrorCodes::CommandNotSupported, "this storage engine does not support touch");
}

Status WiredTigerRecordStore::oplogDiskLocRegister(OperationContext* txn, const Timestamp& opTime) {
    StatusWith<RecordId> id = oploghack::keyForOptime(opTime);
    if (!id.isOK())
        return id.getStatus();

    stdx::lock_guard<stdx::mutex> lk(_uncommittedRecordIdsMutex);
    _addUncommittedRecordId_inlock(txn, id.getValue());
    return Status::OK();
}

class WiredTigerRecordStore::CappedInsertChange : public RecoveryUnit::Change {
public:
    CappedInsertChange(WiredTigerRecordStore* rs, SortedRecordIds::iterator it)
        : _rs(rs), _it(it) {}

    virtual void commit() {
        _rs->_dealtWithCappedId(_it, true);
        // Do not notify here because all committed inserts notify, always.
    }

    virtual void rollback() {
        // Notify on rollback since it might make later commits visible.
        _rs->_dealtWithCappedId(_it, false);
        stdx::lock_guard<stdx::mutex> lk(_rs->_cappedCallbackMutex);
        if (_rs->_cappedCallback)
            _rs->_cappedCallback->notifyCappedWaitersIfNeeded();
    }

private:
    WiredTigerRecordStore* const _rs;
    const SortedRecordIds::iterator _it;
};

void WiredTigerRecordStore::_oplogJournalThreadLoop(WiredTigerSessionCache* sessionCache) try {
    Client::initThread("WTOplogJournalThread");
    while (true) {
        stdx::unique_lock<stdx::mutex> lk(_uncommittedRecordIdsMutex);
        _opsWaitingForJournalCV.wait(
            lk, [&] { return _shuttingDown || !_opsWaitingForJournal.empty(); });

        while (!_shuttingDown && MONGO_FAIL_POINT(WTPausePrimaryOplogDurabilityLoop)) {
            lk.unlock();
            sleepmillis(10);
            lk.lock();
        }

        if (_shuttingDown)
            return;

        decltype(_opsWaitingForJournal) opsAboutToBeJournaled = {};
        _opsWaitingForJournal.swap(opsAboutToBeJournaled);

        lk.unlock();
        sessionCache->waitUntilDurable(/*forceCheckpoint=*/false);
        lk.lock();

        for (auto&& op : opsAboutToBeJournaled) {
            _uncommittedRecordIds.erase(op);
        }

        _opsBecameVisibleCV.notify_all();
        lk.unlock();

        stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
        if (_cappedCallback) {
            _cappedCallback->notifyCappedWaitersIfNeeded();
        }
    }
} catch (...) {
    std::terminate();
}

void WiredTigerRecordStore::waitForAllEarlierOplogWritesToBeVisible(OperationContext* txn) const {
    invariant(txn->lockState()->isNoop() || !txn->lockState()->inAWriteUnitOfWork());

    // This function must not start a WT transaction, otherwise we will get stuck in an infinite
    // loop of WCE handling when the getCursor() is called.

    stdx::unique_lock<stdx::mutex> lk(_uncommittedRecordIdsMutex);
    const auto waitingFor = _oplog_highestSeen;
    while (!_uncommittedRecordIds.empty() && _uncommittedRecordIds.front() <= waitingFor) {
        // We can't use a simple wait() here because we need to wake up periodically to check for
        // interrupt and OperationContext::waitForConditionOrInterrupt doesn't exist on this branch.
        txn->checkForInterrupt();
        _opsBecameVisibleCV.wait_for(lk, Microseconds(Seconds(10)));
    }
}

void WiredTigerRecordStore::_addUncommittedRecordId_inlock(OperationContext* txn, RecordId id) {
    dassert(_uncommittedRecordIds.empty() || _uncommittedRecordIds.back() < id);
    SortedRecordIds::iterator it = _uncommittedRecordIds.insert(_uncommittedRecordIds.end(), id);
    invariant(it->isNormal());
    txn->recoveryUnit()->registerChange(new CappedInsertChange(this, it));
    _oplog_highestSeen = id;
}

boost::optional<RecordId> WiredTigerRecordStore::oplogStartHack(
    OperationContext* txn, const RecordId& startingPosition) const {
    if (!_useOplogHack)
        return boost::none;

    {
        WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(txn);
        _oplogSetStartHack(wru);
    }

    WiredTigerCursor cursor(_uri, _tableId, true, txn);
    WT_CURSOR* c = cursor.get();

    int cmp;
    c->set_key(c, _makeKey(startingPosition));
    int ret = WT_READ_CHECK(c->search_near(c, &cmp));
    if (ret == 0 && cmp > 0)
        ret = c->prev(c);  // landed one higher than startingPosition
    if (ret == WT_NOTFOUND)
        return RecordId();  // nothing <= startingPosition
    invariantWTOK(ret);

    int64_t key;
    ret = c->get_key(c, &key);
    invariantWTOK(ret);
    return _fromKey(key);
}

void WiredTigerRecordStore::updateStatsAfterRepair(OperationContext* txn,
                                                   long long numRecords,
                                                   long long dataSize) {
    _numRecords.store(numRecords);
    _dataSize.store(dataSize);

    if (_sizeStorer) {
        _sizeStorer->storeToCache(_uri, numRecords, dataSize);
    }
}

RecordId WiredTigerRecordStore::_nextId() {
    invariant(!_useOplogHack);
    RecordId out = RecordId(_nextIdNum.fetchAndAdd(1));
    invariant(out.isNormal());
    return out;
}

WiredTigerRecoveryUnit* WiredTigerRecordStore::_getRecoveryUnit(OperationContext* txn) {
    return checked_cast<WiredTigerRecoveryUnit*>(txn->recoveryUnit());
}

class WiredTigerRecordStore::NumRecordsChange : public RecoveryUnit::Change {
public:
    NumRecordsChange(WiredTigerRecordStore* rs, int64_t diff) : _rs(rs), _diff(diff) {}
    virtual void commit() {}
    virtual void rollback() {
        _rs->_numRecords.fetchAndAdd(-_diff);
    }

private:
    WiredTigerRecordStore* _rs;
    int64_t _diff;
};

void WiredTigerRecordStore::_changeNumRecords(OperationContext* txn, int64_t diff) {
    txn->recoveryUnit()->registerChange(new NumRecordsChange(this, diff));
    if (_numRecords.fetchAndAdd(diff) < 0)
        _numRecords.store(std::max(diff, int64_t(0)));
}

class WiredTigerRecordStore::DataSizeChange : public RecoveryUnit::Change {
public:
    DataSizeChange(WiredTigerRecordStore* rs, int64_t amount) : _rs(rs), _amount(amount) {}
    virtual void commit() {}
    virtual void rollback() {
        _rs->_increaseDataSize(NULL, -_amount);
    }

private:
    WiredTigerRecordStore* _rs;
    int64_t _amount;
};

void WiredTigerRecordStore::_increaseDataSize(OperationContext* txn, int64_t amount) {
    if (txn)
        txn->recoveryUnit()->registerChange(new DataSizeChange(this, amount));

    if (_dataSize.fetchAndAdd(amount) < 0)
        _dataSize.store(std::max(amount, int64_t(0)));

    if (_sizeStorer && _sizeStorerCounter++ % 1000 == 0) {
        _sizeStorer->storeToCache(_uri, _numRecords.load(), _dataSize.load());
    }
}

int64_t WiredTigerRecordStore::_makeKey(const RecordId& id) {
    return id.repr();
}
RecordId WiredTigerRecordStore::_fromKey(int64_t key) {
    return RecordId(key);
}

void WiredTigerRecordStore::temp_cappedTruncateAfter(OperationContext* txn,
                                                     RecordId end,
                                                     bool inclusive) {
    Cursor cursor(txn, *this);

    auto record = cursor.seekExact(end);
    massert(28807, str::stream() << "Failed to seek to the record located at " << end, record);

    int64_t recordsRemoved = 0;
    int64_t bytesRemoved = 0;
    RecordId lastKeptId;
    RecordId firstRemovedId;

    if (inclusive) {
        Cursor reverseCursor(txn, *this, false);
        invariant(reverseCursor.seekExact(end));
        auto prev = reverseCursor.next();
        lastKeptId = prev ? prev->id : RecordId();
        firstRemovedId = end;
    } else {
        // If not deleting the record located at 'end', then advance the cursor to the first record
        // that is being deleted.
        record = cursor.next();
        if (!record) {
            return;  // No records to delete.
        }
        lastKeptId = end;
        firstRemovedId = record->id;
    }

    // Compute the number and associated sizes of the records to delete.
    {
        stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
        do {
            if (_cappedCallback) {
                uassertStatusOK(
                    _cappedCallback->aboutToDeleteCapped(txn, record->id, record->data));
            }
            recordsRemoved++;
            bytesRemoved += record->data.size();
        } while ((record = cursor.next()));
    }

    // Truncate the collection starting from the record located at 'firstRemovedId' to the end of
    // the collection.
    WriteUnitOfWork wuow(txn);

    WiredTigerCursor startwrap(_uri, _tableId, true, txn);
    WT_CURSOR* start = startwrap.get();
    start->set_key(start, _makeKey(firstRemovedId));

    WT_SESSION* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn)->getSession();
    invariantWTOK(session->truncate(session, nullptr, start, nullptr, nullptr));

    _changeNumRecords(txn, -recordsRemoved);
    _increaseDataSize(txn, -bytesRemoved);

    wuow.commit();

    if (_useOplogHack) {
        // Forget that we've ever seen a higher timestamp than we now have.
        _oplog_highestSeen = lastKeptId;
    }

    if (_oplogStones) {
        _oplogStones->updateStonesAfterCappedTruncateAfter(
            recordsRemoved, bytesRemoved, firstRemovedId);
    }
}

}  // namespace mongo
