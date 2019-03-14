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

#include <boost/optional.hpp>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/optime_pair.h"

namespace mongo {

class ActionLogType;
class BatchedCommandRequest;
class BatchedCommandResponse;
struct BSONArray;
class BSONArrayBuilder;
class BSONObj;
class BSONObjBuilder;
class ChunkType;
struct ChunkVersion;
class CollectionType;
class ConnectionString;
class DatabaseType;
class DistLockManager;
class NamespaceString;
class OperationContext;
class SettingsType;
class ShardKeyPattern;
class ShardRegistry;
class ShardType;
class Status;
template <typename T>
class StatusWith;
class TagsType;

/**
 * Used to indicate to the caller of the removeShard method whether draining of chunks for
 * a particular shard has started, is ongoing, or has been completed.
 */
enum ShardDrainingStatus {
    STARTED,
    ONGOING,
    COMPLETED,
};

/**
 * Abstracts reads and writes of the sharding catalog metadata.
 *
 * All implementations of this interface should go directly to the persistent backing store
 * and should avoid doing any caching of their own. The caching is delegated to a parallel
 * read-only view of the catalog, which is maintained by a higher level code.
 */
class CatalogManager {
    MONGO_DISALLOW_COPYING(CatalogManager);
    friend class ForwardingCatalogManager;

public:
    enum class ConfigServerMode {
        NONE,
        SCCC,
        CSRS,
    };

    virtual ~CatalogManager() = default;

    /**
     * Performs implementation-specific startup tasks. Must be run after the catalog manager
     * has been installed into the global 'grid' object. Implementation do not need to guarantee
     * thread safety so callers should employ proper synchronization when calling this method.
     */
    virtual Status startup(OperationContext* txn, bool allowNetworking) = 0;

    /**
     * Performs necessary cleanup when shutting down cleanly.
     */
    virtual void shutDown(OperationContext* txn, bool allowNetworking = true) = 0;

    /**
     * Returns what type of catalog manager this is - CSRS for the CatalogManagerReplicaSet and
     * SCCC for the CatalogManagerLegacy.
     */
    virtual ConfigServerMode getMode() = 0;

    /**
     * Creates a new database or updates the sharding status for an existing one. Cannot be
     * used for the admin/config/local DBs, which should not be created or sharded manually
     * anyways.
     *
     * Returns Status::OK on success or any error code indicating the failure. These are some
     * of the known failures:
     *  - DatabaseDifferCase - database already exists, but with a different case
     *  - ShardNotFound - could not find a shard to place the DB on
     */
    virtual Status enableSharding(OperationContext* txn, const std::string& dbName) = 0;

    /**
     * Shards a collection. Assumes that the database is enabled for sharding.
     *
     * @param ns: namespace of collection to shard
     * @param fieldsAndOrder: shardKey pattern
     * @param unique: if true, ensure underlying index enforces a unique constraint.
     * @param initPoints: create chunks based on a set of specified split points.
     * @param initShardIds: If non-empty, specifies the set of shards to assign chunks between.
     *     Otherwise all chunks will be assigned to the primary shard for the database.
     *
     * WARNING: It's not completely safe to place initial chunks onto non-primary
     *          shards using this method because a conflict may result if multiple map-reduce
     *          operations are writing to the same output collection, for instance.
     *
     */
    virtual Status shardCollection(OperationContext* txn,
                                   const std::string& ns,
                                   const ShardKeyPattern& fieldsAndOrder,
                                   bool unique,
                                   const std::vector<BSONObj>& initPoints,
                                   const std::set<ShardId>& initShardIds) = 0;

    /**
     *
     * Adds a new shard. It expects a standalone mongod process or replica set to be running
     * on the provided address.
     *
     * @param  shardProposedName is an optional string with the proposed name of the shard.
     *         If it is nullptr, a name will be automatically generated; if not nullptr, it cannot
     *         contain the empty string.
     * @param  shardConnectionString is the connection string of the shard being added.
     * @param  maxSize is the optional space quota in bytes. Zeros means there's
     *         no limitation to space usage.
     * @return either an !OK status or the name of the newly added shard.
     */
    virtual StatusWith<std::string> addShard(OperationContext* txn,
                                             const std::string* shardProposedName,
                                             const ConnectionString& shardConnectionString,
                                             const long long maxSize) = 0;

    /**
     * Tries to remove a shard. To completely remove a shard from a sharded cluster,
     * the data residing in that shard must be moved to the remaining shards in the
     * cluster by "draining" chunks from that shard.
     *
     * Because of the asynchronous nature of the draining mechanism, this method returns
     * the current draining status. See ShardDrainingStatus enum definition for more details.
     */
    virtual StatusWith<ShardDrainingStatus> removeShard(OperationContext* txn,
                                                        const std::string& name) = 0;

    /**
     * Updates or creates the metadata for a given database.
     */
    virtual Status updateDatabase(OperationContext* txn,
                                  const std::string& dbName,
                                  const DatabaseType& db) = 0;

    /**
     * Retrieves the metadata for a given database, if it exists.
     *
     * @param dbName name of the database (case sensitive)
     *
     * Returns Status::OK along with the database information and the OpTime of the config server
     * which the database information was based upon. Otherwise, returns an error code indicating
     * the failure. These are some of the known failures:
     *  - NamespaceNotFound - database does not exist
     */
    virtual StatusWith<OpTimePair<DatabaseType>> getDatabase(OperationContext* txn,
                                                             const std::string& dbName) = 0;

    /**
     * Updates or creates the metadata for a given collection.
     */
    virtual Status updateCollection(OperationContext* txn,
                                    const std::string& collNs,
                                    const CollectionType& coll) = 0;

    /**
     * Retrieves the metadata for a given collection, if it exists.
     *
     * @param collectionNs fully qualified name of the collection (case sensitive)
     *
     * Returns Status::OK along with the collection information and the OpTime of the config server
     * which the collection information was based upon. Otherwise, returns an error code indicating
     * the failure. These are some of the known failures:
     *  - NamespaceNotFound - collection does not exist
     */
    virtual StatusWith<OpTimePair<CollectionType>> getCollection(OperationContext* txn,
                                                                 const std::string& collNs) = 0;

    /**
     * Retrieves all collections undera specified database (or in the system).
     *
     * @param dbName an optional database name. Must be nullptr or non-empty. If nullptr is
     *      specified, all collections on the system are returned.
     * @param collections variable to receive the set of collections.
     * @param optime an out parameter that will contain the opTime of the config server.
     *      Can be null. Note that collections can be fetched in multiple batches and each batch
     *      can have a unique opTime. This opTime will be the one from the last batch.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual Status getCollections(OperationContext* txn,
                                  const std::string* dbName,
                                  std::vector<CollectionType>* collections,
                                  repl::OpTime* optime) = 0;

    /**
     * Drops the specified collection from the collection metadata store.
     *
     * Returns Status::OK if successful or any error code indicating the failure. These are
     * some of the known failures:
     *  - NamespaceNotFound - collection does not exist
     */
    virtual Status dropCollection(OperationContext* txn, const NamespaceString& ns) = 0;

    /**
     * Retrieves all databases for a shard.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual Status getDatabasesForShard(OperationContext* txn,
                                        const std::string& shardName,
                                        std::vector<std::string>* dbs) = 0;

    /**
     * Gets the requested number of chunks (of type ChunkType) that satisfy a query.
     *
     * @param filter The query to filter out the results.
     * @param sort Fields to use for sorting the results. Pass empty BSON object for no sort.
     * @param limit The number of chunk entries to return. Pass boost::none for no limit.
     * @param chunks Vector entry to receive the results
     * @param optime an out parameter that will contain the opTime of the config server.
     *      Can be null. Note that chunks can be fetched in multiple batches and each batch
     *      can have a unique opTime. This opTime will be the one from the last batch.
     *
     * Returns a !OK status if an error occurs.
     */
    virtual Status getChunks(OperationContext* txn,
                             const BSONObj& filter,
                             const BSONObj& sort,
                             boost::optional<int> limit,
                             std::vector<ChunkType>* chunks,
                             repl::OpTime* opTime) = 0;

    /**
     * Retrieves all tags for the specified collection.
     */
    virtual Status getTagsForCollection(OperationContext* txn,
                                        const std::string& collectionNs,
                                        std::vector<TagsType>* tags) = 0;

    /**
     * Retrieves the most appropriate tag, which overlaps with the specified chunk. If no tags
     * overlap, returns an empty string.
     */
    virtual StatusWith<std::string> getTagForChunk(OperationContext* txn,
                                                   const std::string& collectionNs,
                                                   const ChunkType& chunk) = 0;

    /**
     * Retrieves all shards in this sharded cluster.
     * Returns a !OK status if an error occurs.
     */
    virtual StatusWith<OpTimePair<std::vector<ShardType>>> getAllShards(OperationContext* txn) = 0;

    /**
     * Runs a user management command on the config servers, potentially synchronizing through
     * a distributed lock. Do not use for general write command execution.
     *
     * @param commandName: name of command
     * @param dbname: database for which the user management command is invoked
     * @param cmdObj: command obj
     * @param result: contains data returned from config servers
     * Returns true on success.
     */
    virtual bool runUserManagementWriteCommand(OperationContext* txn,
                                               const std::string& commandName,
                                               const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               BSONObjBuilder* result) = 0;

    /**
     * Runs a user management related read-only command on a config server.
     */
    virtual bool runUserManagementReadCommand(OperationContext* txn,
                                              const std::string& dbname,
                                              const BSONObj& cmdObj,
                                              BSONObjBuilder* result) = 0;

    /**
     * Applies oplog entries to the config servers.
     * Used by mergeChunk, splitChunk, and moveChunk commands.
     *
     * @param updateOps: documents to write to the chunks collection.
     * @param preCondition: preconditions for applying documents.
     * @param nss: namespace string for the chunks collection.
     * @param lastChunkVersion: version of the last document being written to the chunks
     * collection.
     *
     * 'nss' and 'lastChunkVersion' uniquely identify the last document being written, which is
     * expected to appear in the chunks collection on success. This is important for the
     * case where network problems cause a retry of a successful write, which then returns
     * failure because the precondition no longer matches. If a query of the chunks collection
     * returns a document matching both 'nss' and 'lastChunkVersion,' the write succeeded.
     */
    virtual Status applyChunkOpsDeprecated(OperationContext* txn,
                                           const BSONArray& updateOps,
                                           const BSONArray& preCondition,
                                           const std::string& nss,
                                           const ChunkVersion& lastChunkVersion) = 0;

    /**
     * Writes a diagnostic event to the action log.
     */
    virtual Status logAction(OperationContext* txn,
                             const std::string& what,
                             const std::string& ns,
                             const BSONObj& detail) = 0;

    /**
     * Writes a diagnostic event to the change log.
     */
    virtual Status logChange(OperationContext* txn,
                             const std::string& what,
                             const std::string& ns,
                             const BSONObj& detail) = 0;

    /**
     * Returns global settings for a certain key.
     * @param key: key for SettingsType::ConfigNS document.
     *
     * Returns ErrorCodes::NoMatchingDocument if no SettingsType::ConfigNS document
     * with such key exists.
     * Returns ErrorCodes::FailedToParse if we encountered an error while parsing
     * the settings document.
     */
    virtual StatusWith<SettingsType> getGlobalSettings(OperationContext* txn,
                                                       const std::string& key) = 0;

    /**
     * Directly sends the specified command to the config server and returns the response.
     *
     * NOTE: Usage of this function is disallowed in new code, which should instead go through
     *       the regular catalog management calls. It is currently only used privately by this
     *       class and externally for writes to the admin/config namespaces.
     *
     * @param request Request to be sent to the config server.
     * @param response Out parameter to receive the response. Can be nullptr.
     */
    virtual void writeConfigServerDirect(OperationContext* txn,
                                         const BatchedCommandRequest& request,
                                         BatchedCommandResponse* response) = 0;

    /**
     * Creates a new database entry for the specified database name in the configuration
     * metadata and sets the specified shard as primary.
     *
     * @param dbName name of the database (case sensitive)
     *
     * Returns Status::OK on success or any error code indicating the failure. These are some
     * of the known failures:
     *  - NamespaceExists - database already exists
     *  - DatabaseDifferCase - database already exists, but with a different case
     *  - ShardNotFound - could not find a shard to place the DB on
     */
    virtual Status createDatabase(OperationContext* txn, const std::string& dbName) = 0;

    /**
     * Directly inserts a document in the specified namespace on the config server. The document
     * must have an _id index. Must only be used for insertions in the 'config' database.
     *
     * NOTE: Should not be used in new code. Instead add a new metadata operation to the interface.
     */
    virtual Status insertConfigDocument(OperationContext* txn,
                                        const std::string& ns,
                                        const BSONObj& doc) = 0;

    /**
     * Updates a single document in the specified namespace on the config server. The document must
     * have an _id index. Must only be used for updates to the 'config' database.
     *
     * This method retries the operation on NotMaster or network errors, so it should only be used
     * with modifications which are idempotent.
     *
     * Returns non-OK status if the command failed to run for some reason. If the command was
     * successful, returns true if a document was actually modified (that is, it did not exist and
     * was upserted or it existed and any of the fields changed) and false otherwise (basically
     * returns whether the update command's response update.n value is > 0).
     *
     * NOTE: Should not be used in new code. Instead add a new metadata operation to the interface.
     */
    virtual StatusWith<bool> updateConfigDocument(OperationContext* txn,
                                                  const std::string& ns,
                                                  const BSONObj& query,
                                                  const BSONObj& update,
                                                  bool upsert) = 0;

    /**
     * Removes documents matching a particular query predicate from the specified namespace on the
     * config server. Must only be used for deletions from the 'config' database.
     *
     * NOTE: Should not be used in new code. Instead add a new metadata operation to the interface.
     */
    virtual Status removeConfigDocuments(OperationContext* txn,
                                         const std::string& ns,
                                         const BSONObj& query) = 0;

    /**
     * Performs the necessary checks for version compatibility and creates a new version document
     * if the current cluster config is empty.
     */
    virtual Status initConfigVersion(OperationContext* txn) = 0;

    /**
     * Appends the information about the config and admin databases in the config server
     * with the format for listDatabase.
     */
    virtual Status appendInfoForConfigServerDatabases(OperationContext* txn,
                                                      BSONArrayBuilder* builder) = 0;

    /**
     * Returns true if all config servers are consistent. Should always return true except for
     * implementation for SCCC.
     */
    virtual bool isMetadataConsistentFromLastCheck(OperationContext* txn) = 0;

    /**
     * Obtains a reference to the distributed lock manager instance to use for synchronizing
     * system-wide changes.
     *
     * The returned reference is valid only as long as the catalog manager is valid and should not
     * be cached.
     */
    virtual DistLockManager* getDistLockManager() = 0;

protected:
    CatalogManager() = default;
};

}  // namespace mongo
