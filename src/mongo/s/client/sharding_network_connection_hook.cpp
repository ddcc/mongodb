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

#include "mongo/s/client/sharding_network_connection_hook.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

Status ShardingNetworkConnectionHook::validateHost(
    const HostAndPort& remoteHost, const executor::RemoteCommandResponse& isMasterReply) {
    return validateHostImpl(remoteHost, isMasterReply, false);
}

Status ShardingNetworkConnectionHook::validateHostImpl(
    const HostAndPort& remoteHost,
    const executor::RemoteCommandResponse& isMasterReply,
    bool forSCC) {
    auto shard = grid.shardRegistry()->getShardForHostNoReload(remoteHost);
    if (!shard) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "No shard found for host: " << remoteHost.toString()};
    }

    long long configServerModeNumber;
    auto status = bsonExtractIntegerField(isMasterReply.data, "configsvr", &configServerModeNumber);

    switch (status.code()) {
        case ErrorCodes::OK: {
            // The ismaster response indicates remoteHost is a config server.
            if (!shard->isConfig()) {
                return {ErrorCodes::InvalidOptions,
                        str::stream() << "Surprised to discover that " << remoteHost.toString()
                                      << " believes it is a config server"};
            }
            using ConfigServerMode = CatalogManager::ConfigServerMode;
            auto configServerMode =
                (configServerModeNumber == 0 ? ConfigServerMode::SCCC : ConfigServerMode::CSRS);

            if (configServerMode == ConfigServerMode::CSRS) {
                uassert(ErrorCodes::ReplicaSetNotFound,
                        "CSRS replica set is not initialized",
                        isMasterReply.data.hasField("setName"));
            }

            const BSONElement setName = isMasterReply.data["setName"];
            // We still want to call scheduleReplaceCatalogManagerIfNeeded when configServerMode
            // is SCCC to catch illegal downgrade attempts and return a useful error message.
            // To enable that we use the default (invalid) ConnectionString when configServerMode
            // is SCCC.
            ConnectionString configConnString;
            if (configServerMode == ConfigServerMode::CSRS) {
                configConnString =
                    ConnectionString::forReplicaSet(setName.valueStringData(), {remoteHost});
            }

            auto catalogSwapStatus =
                grid.forwardingCatalogManager()->scheduleReplaceCatalogManagerIfNeeded(
                    configServerMode, configConnString);
            if (configServerMode == ConfigServerMode::CSRS && catalogSwapStatus.isOK() && forSCC) {
                // Even though scheduleReplaceCatalogManagerIfNeeded didn't indicate that a catalog
                // manager swap is needed, if this connection is part of a SyncClusterConnection,
                // and it's talking to a CSRS config server we still need to fail.
                return Status(ErrorCodes::IncompatibleCatalogManager,
                              "Need to swap sharding catalog manager. Detected config server in "
                              "CSRS mode while using a SyncClusterConnection, which only supports "
                              "SCCC mode config servers");
            }
            return catalogSwapStatus;
        }
        case ErrorCodes::NoSuchKey: {
            // The ismaster response indicates that remoteHost is not a config server, or that
            // the config server is running a version prior to the 3.1 development series.
            if (!shard->isConfig()) {
                return Status::OK();
            }
            long long remoteMaxWireVersion;
            status = bsonExtractIntegerFieldWithDefault(isMasterReply.data,
                                                        "maxWireVersion",
                                                        RELEASE_2_4_AND_BEFORE,
                                                        &remoteMaxWireVersion);
            if (!status.isOK()) {
                return status;
            }
            if (remoteMaxWireVersion < FIND_COMMAND) {
                // Prior to the introduction of the find command and the 3.1 release series, it was
                // not possible to distinguish a config server from a shard server from its ismaster
                // response. As such, we must assume that the system is properly configured.
                return Status::OK();
            }
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Surprised to discover that " << remoteHost.toString()
                                  << " does not believe it is a config server"};
        }
        default:
            // The ismaster response was malformed.
            return status;
    }
}

StatusWith<boost::optional<executor::RemoteCommandRequest>>
ShardingNetworkConnectionHook::makeRequest(const HostAndPort& remoteHost) {
    auto shard = grid.shardRegistry()->getShardForHostNoReload(remoteHost);
    if (!shard) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "No shard found for host: " << remoteHost.toString()};
    }
    if (shard->isConfig()) {
        // No need to initialize sharding metadata if talking to a config server
        return {boost::none};
    }

    SetShardVersionRequest ssv = SetShardVersionRequest::makeForInitNoPersist(
        grid.shardRegistry()->getConfigServerConnectionString(),
        shard->getId(),
        shard->getConnString());
    executor::RemoteCommandRequest request;
    request.dbname = "admin";
    request.target = remoteHost;
    request.timeout = stdx::chrono::seconds{30};
    request.cmdObj = ssv.toBSON();

    return {request};
}

Status ShardingNetworkConnectionHook::handleReply(const HostAndPort& remoteHost,
                                                  executor::RemoteCommandResponse&& response) {
    return getStatusFromCommandResult(response.data);
}
}  // namespace mongo
