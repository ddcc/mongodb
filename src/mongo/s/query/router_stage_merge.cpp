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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/router_stage_merge.h"

#include "mongo/util/scopeguard.h"

namespace mongo {

RouterStageMerge::RouterStageMerge(executor::TaskExecutor* executor,
                                   ClusterClientCursorParams&& params)
    : _executor(executor), _arm(executor, std::move(params)) {}

StatusWith<boost::optional<BSONObj>> RouterStageMerge::next() {
    while (!_arm.ready()) {
        auto nextEventStatus = _arm.nextEvent();
        if (!nextEventStatus.isOK()) {
            return nextEventStatus.getStatus();
        }
        auto event = nextEventStatus.getValue();

        // Block until there are further results to return.
        _executor->waitForEvent(event);
    }

    return _arm.nextReady();
}

void RouterStageMerge::kill() {
    auto killEvent = _arm.kill();
    if (!killEvent) {
        // Mongos is shutting down.
        return;
    }
    _executor->waitForEvent(killEvent);
}

bool RouterStageMerge::remotesExhausted() {
    return _arm.remotesExhausted();
}

Status RouterStageMerge::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    return _arm.setAwaitDataTimeout(awaitDataTimeout);
}

}  // namespace mongo
