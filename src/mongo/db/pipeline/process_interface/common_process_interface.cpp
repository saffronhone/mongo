/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <boost/optional.hpp>
#include <cstddef>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/curop.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/query/client_cursor/generic_cursor_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/router_role.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

std::vector<BSONObj> CommonProcessInterface::getCurrentOps(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    CurrentOpConnectionsMode connMode,
    CurrentOpSessionsMode sessionMode,
    CurrentOpUserMode userMode,
    CurrentOpTruncateMode truncateMode,
    CurrentOpCursorMode cursorMode) const {
    OperationContext* opCtx = expCtx->getOperationContext();
    AuthorizationSession* ctxAuth = AuthorizationSession::get(opCtx->getClient());

    std::vector<BSONObj> ops;

    auto reportCurrentOpForService = [&](Service* service) {
        for (Service::LockedClientsCursor cursor(service); ClientLock lc = cursor.next();) {
            Client* client = &*lc;
            if (AuthorizationManager::get(opCtx->getService())->isAuthEnabled()) {
                // If auth is disabled, ignore the allUsers parameter.
                if (userMode == CurrentOpUserMode::kExcludeOthers &&
                    !ctxAuth->isCoauthorizedWithClient(client, lc)) {
                    continue;
                }

                // If currOp is being run for a particular tenant, ignore any ops that don't belong
                // to it.
                if (auto expCtxTenantId = expCtx->getNamespaceString().tenantId()) {
                    auto userName = AuthorizationSession::get(client)->getAuthenticatedUserName();
                    if ((userName && userName->tenantId() &&
                         userName->tenantId() != expCtxTenantId) ||
                        (userName && !userName->tenantId() &&
                         !CurOp::currentOpBelongsToTenant(client, *expCtxTenantId))) {
                        continue;
                    }
                }
            }

            // Ignore inactive connections unless 'idleConnections' is true.
            if (connMode == CurrentOpConnectionsMode::kExcludeIdle &&
                !client->hasAnyActiveCurrentOp()) {
                continue;
            }

            // Delegate to the mongoD- or mongoS-specific implementation of
            // _reportCurrentOpForClient.
            ops.emplace_back(_reportCurrentOpForClient(expCtx, client, truncateMode));
        }
    };

    if (opCtx->routedByReplicaSetEndpoint()) {
        // On the replica set endpoint, currentOp should report both router and shard operations.
        auto serviceContext = opCtx->getServiceContext();
        reportCurrentOpForService(serviceContext->getService(ClusterRole::RouterServer));
        reportCurrentOpForService(serviceContext->getService(ClusterRole::ShardServer));
    } else {
        reportCurrentOpForService(opCtx->getService());
    }

    // If 'cursorMode' is set to include idle cursors, retrieve them and add them to ops.
    if (cursorMode == CurrentOpCursorMode::kIncludeCursors) {

        for (auto&& cursor : getIdleCursors(expCtx, userMode)) {
            BSONObjBuilder cursorObj;
            cursorObj.append("type", "idleCursor");
            cursorObj.append("host", prettyHostNameAndPort(opCtx->getClient()->getLocalPort()));
            // First, extract fields which need to go at the top level out of the GenericCursor.
            if (auto ns = cursor.getNs()) {
                tassert(7663401,
                        str::stream()
                            << "SerializationContext on the expCtx should not be empty, with ns: "
                            << ns->toStringForErrorMsg(),
                        expCtx->getSerializationContext() != SerializationContext::stateDefault());
                cursorObj.append(
                    "ns", NamespaceStringUtil::serialize(*ns, expCtx->getSerializationContext()));
            } else
                cursorObj.append("ns", "");

            if (auto lsid = cursor.getLsid()) {
                cursorObj.append("lsid", lsid->toBSON());
            }
            if (auto planSummaryData = cursor.getPlanSummary()) {  // Not present on mongos.
                cursorObj.append("planSummary", *planSummaryData);
            }

            // Next, append the stripped-down version of the generic cursor. This will avoid
            // duplicating information reported at the top level.
            cursorObj.append("cursor",
                             CurOp::truncateAndSerializeGenericCursor(cursor, boost::none));

            ops.emplace_back(cursorObj.obj());
        }
    }

    // If we need to report on idle Sessions, defer to the mongoD or mongoS implementations.
    if (sessionMode == CurrentOpSessionsMode::kIncludeIdle) {
        _reportCurrentOpsForIdleSessions(opCtx, userMode, &ops);
    }

    if (!AuthorizationManager::get(opCtx->getService())->isAuthEnabled() ||
        userMode == CurrentOpUserMode::kIncludeAll) {
        _reportCurrentOpsForTransactionCoordinators(
            opCtx, sessionMode == MongoProcessInterface::CurrentOpSessionsMode::kIncludeIdle, &ops);

        _reportCurrentOpsForPrimaryOnlyServices(opCtx, connMode, sessionMode, &ops);

        _reportCurrentOpsForQueryAnalysis(opCtx, &ops);
    }

    return ops;
}

std::vector<FieldPath> CommonProcessInterface::collectDocumentKeyFieldsActingAsRouter(
    OperationContext* opCtx, const NamespaceString& nss) const {
    const auto criSW = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
    if (criSW.isOK() && criSW.getValue().cm.isSharded()) {
        return shardKeyToDocumentKeyFields(
            criSW.getValue().cm.getShardKeyPattern().getKeyPatternFields());
    } else if (!criSW.isOK() && criSW.getStatus().code() != ErrorCodes::NamespaceNotFound) {
        uassertStatusOK(criSW);
    }

    // We have no evidence this collection is sharded, so the document key is just _id.
    return {"_id"};
}

void CommonProcessInterface::updateClientOperationTime(OperationContext* opCtx) const {
    // In order to support causal consistency in a replica set or a sharded cluster when reading
    // with secondary read preference, the secondary must propagate the primary's operation time
    // to the client so that when the client attempts to read, the secondary will block until it
    // has replicated the primary's writes. As such, the 'operationTime' returned from the
    // primary is explicitly set on the given opCtx's client.
    //
    // Note that the operationTime is attached even when a command fails because writes may succeed
    // while the command fails (such as in a $merge where 'whenMatched' is set to fail). This
    // guarantees that the operation time returned to the client reflects the most recent
    // successful write executed by this client.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord) {
        auto operationTime = OperationTimeTracker::get(opCtx)->getMaxOperationTime();
        repl::ReplClientInfo::forClient(opCtx->getClient())
            .setLastProxyWriteTimestampForward(operationTime.asTimestamp());
    }
}

bool CommonProcessInterface::keyPatternNamesExactPaths(const BSONObj& keyPattern,
                                                       const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

boost::optional<mongo::DatabaseVersion> CommonProcessInterface::refreshAndGetDatabaseVersion(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const DatabaseName& dbName) const {

    const auto catalogCache = Grid::get(expCtx->getOperationContext())->catalogCache();
    catalogCache->onStaleDatabaseVersion(dbName, boost::none /* wantedVersion */);
    auto db = catalogCache->getDatabase(expCtx->getOperationContext(), dbName);

    return db.isOK() ? boost::make_optional(db.getValue()->getVersion()) : boost::none;
}

std::vector<FieldPath> CommonProcessInterface::shardKeyToDocumentKeyFields(
    const std::vector<std::unique_ptr<FieldRef>>& keyPatternFields) {
    std::vector<FieldPath> result;
    bool gotId = false;
    for (auto& field : keyPatternFields) {
        result.emplace_back(field->dottedField());
        gotId |= (result.back().fullPath() == "_id");
    }
    if (!gotId) {  // If not part of the shard key, "_id" comes last.
        result.emplace_back("_id");
    }
    return result;
}

boost::optional<ShardId> CommonProcessInterface::findOwningShard(OperationContext* opCtx,
                                                                 const NamespaceString& nss) {
    // Do not attempt to refresh the catalog cache when holding a lock.
    if (shard_role_details::getLocker(opCtx)->isLocked()) {
        return boost::none;
    }
    auto* grid = Grid::get(opCtx);
    tassert(7958000, "Grid should be initialized", grid && grid->isInitialized());

    return CommonProcessInterface::findOwningShard(opCtx, grid->catalogCache(), nss);
}

boost::optional<ShardId> CommonProcessInterface::findOwningShard(OperationContext* opCtx,
                                                                 CatalogCache* catalogCache,
                                                                 const NamespaceString& nss) {
    tassert(7958001, "CatalogCache should be initialized", catalogCache);
    auto swCRI = catalogCache->getCollectionRoutingInfo(opCtx, nss);
    if (swCRI.getStatus().code() == ErrorCodes::NamespaceNotFound) {
        return boost::none;
    }
    auto [cm, _] = uassertStatusOK(swCRI);

    if (cm.hasRoutingTable()) {
        if (cm.isUnsplittable()) {
            return cm.getMinKeyShardIdWithSimpleCollation();
        } else {
            return boost::none;
        }
    } else {
        return cm.dbPrimary();
    }
    return boost::none;
}

std::string CommonProcessInterface::getHostAndPort(OperationContext* opCtx) const {
    return prettyHostNameAndPort(opCtx->getClient()->getLocalPort());
}

std::vector<DatabaseName> CommonProcessInterface::_getAllDatabasesOnAShardedCluster(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    tassert(9525808, "This method can only run on a sharded cluster", Grid::get(opCtx));

    const std::vector<DatabaseType> databaseTypes = Grid::get(opCtx)->catalogClient()->getAllDBs(
        opCtx, repl::ReadConcernLevel::kSnapshotReadConcern);

    std::vector<DatabaseName> databases;
    databases.reserve(databaseTypes.size());

    std::transform(databaseTypes.begin(),
                   databaseTypes.end(),
                   std::back_inserter(databases),
                   [](const DatabaseType& dbType) -> DatabaseName { return dbType.getDbName(); });

    return databases;
}

std::vector<BSONObj> CommonProcessInterface::_runListCollectionsCommandOnAShardedCluster(
    OperationContext* opCtx, const NamespaceString& nss, bool addPrimaryShard) {
    tassert(9525809, "This method can only run on a sharded cluster", Grid::get(opCtx));

    sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), nss.dbName());
    const bool isCollectionless = nss.coll().empty();
    return router.route(
        opCtx,
        "CommonMongodProcessInterface::_runListCollectionsCommandOnAShardedCluster",
        [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
            ListCollections listCollectionsCmd;
            listCollectionsCmd.setDbName(nss.dbName());
            if (!isCollectionless) {
                listCollectionsCmd.setFilter(BSON("name" << nss.coll()));
            }
            generic_argument_util::setDbVersionIfPresent(listCollectionsCmd, cdb->getVersion());

            const auto shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cdb->getPrimary()));
            Shard::QueryResponse resultCollections;

            try {
                resultCollections = uassertStatusOK(shard->runExhaustiveCursorCommand(
                    opCtx,
                    ReadPreferenceSetting::get(opCtx),
                    nss.dbName(),
                    listCollectionsCmd.toBSON(),
                    opCtx->hasDeadline() ? opCtx->getRemainingMaxTimeMillis() : Milliseconds(-1)));
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                return std::vector<BSONObj>();
            }

            auto addPrimaryField = [&cdb](BSONObj obj) {
                BSONObjBuilder bob(obj.getOwned());
                bob.append("primary", cdb->getPrimary());
                return bob.obj();
            };

            if (isCollectionless) {
                if (addPrimaryShard) {
                    std::vector<BSONObj> collections;
                    collections.reserve(resultCollections.docs.size());
                    for (BSONObj& bsonObj : resultCollections.docs) {
                        collections.emplace_back(addPrimaryField(bsonObj.getOwned()));
                    }
                    return collections;
                }
                return resultCollections.docs;
            }

            for (BSONObj& bsonObj : resultCollections.docs) {
                // Return the entire 'listCollections' response for the first element which matches
                // on name.
                const BSONElement nameElement = bsonObj["name"];
                if (!nameElement || nameElement.valueStringDataSafe() != nss.coll()) {
                    continue;
                }

                return (addPrimaryShard ? std::vector<BSONObj>{addPrimaryField(bsonObj.getOwned())}
                                        : std::vector<BSONObj>{bsonObj.getOwned()});

                tassert(5983900,
                        str::stream()
                            << "Expected at most one collection with the name "
                            << nss.toStringForErrorMsg() << ": " << resultCollections.docs.size(),
                        resultCollections.docs.size() <= 1);
            }

            return std::vector<BSONObj>();
        });
}

}  // namespace mongo
