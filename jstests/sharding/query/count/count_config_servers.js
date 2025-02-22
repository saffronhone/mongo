/**
 * Test count commands against the config servers, including when some of them are down.
 * This test fails when run with authentication due to SERVER-6327
 * @tags: [
 *   # TODO (SERVER-97257): Re-enable this test.
 *   # Test doesn't start enough mongods to have num_mongos routers
 *   embedded_router_incompatible,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Checking UUID and index consistency requires querying the config primary, but this test
// shuts down 2 out of the 3 config servers. Therefore, we cannot do the check on this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

var st = new ShardingTest({name: 'sync_conn_cmd', shards: TestData.configShard ? 1 : 0, config: 3});
st.s.setSecondaryOk();

// The combination of config shard and replica set endpoint makes listIndexes go through the
// sharding code, which can not route it after the test shuts down 2 out of the 3 config servers.
if (TestData.configShard && FeatureFlagUtil.isEnabled(st.s, "ReplicaSetEndpoint")) {
    TestData.skipCollectionAndIndexValidation = true;
}

var configDB = st.config;
var coll = configDB.test;

for (var x = 0; x < 10; x++) {
    assert.commandWorked(coll.insert({v: x}));
}

if (st.configRS) {
    // Make sure the inserts are replicated to all config servers.
    st.configRS.awaitReplication();
}

var testNormalCount = function() {
    var cmdRes = configDB.runCommand({count: coll.getName()});
    assert(cmdRes.ok);
    assert.eq(10, cmdRes.n);
};

var testCountWithQuery = function() {
    var cmdRes = configDB.runCommand({count: coll.getName(), query: {v: {$gt: 6}}});
    assert(cmdRes.ok);
    assert.eq(3, cmdRes.n);
};

// Use invalid query operator to make the count return error
var testInvalidCount = function() {
    var cmdRes = configDB.runCommand({count: coll.getName(), query: {$c: {$abc: 3}}});
    assert(!cmdRes.ok);
    assert(cmdRes.errmsg.length > 0);
};

// Test with all config servers up
testNormalCount();
testCountWithQuery();
testInvalidCount();

// Test with the first config server down
MongoRunner.stopMongod(st.c0);

testNormalCount();
testCountWithQuery();
testInvalidCount();

// Test with the first and second config server down
MongoRunner.stopMongod(st.c1);
jsTest.log('Second server is down');

testNormalCount();
testCountWithQuery();
testInvalidCount();

st.stop();
