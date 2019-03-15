/**
 * 1. check top numbers are correct
 *
 * This test attempts to perform read operations and get statistics using the top command. The
 * former operation may be routed to a secondary in the replica set, whereas the latter must be
 * routed to the primary.
 *
 * @tags: [assumes_read_preference_unchanged]
 */

var name = "toptest";

var testDB = db.getSiblingDB(name);
var testColl = testDB[name + "coll"];

//  Ensure an empty collection exists for first top command
testColl.drop();
testColl.insert({x: 0});
testColl.remove({x: 0});

// get top statistics for the test collection
function getTop() {
    return testDB.adminCommand("top").totals[testColl.getFullName()];
}

//  This variable is used to get differential output
var lastTop = getTop();

//  return the number of operations since the last call to diffTop for the specified key
function diffTop(key) {
    var thisTop = getTop();
    difference = {
        time: thisTop[key].time - lastTop[key].time,
        count: thisTop[key].count - lastTop[key].count
    };
    lastTop[key] = thisTop[key];

    assert.gte(difference.count, 0, "non-decreasing count");
    assert.gte(difference.time, 0, "non-decreasing time");

    //  Time should advance iff operations were performed
    assert.eq(difference.count != 0, difference.time > 0, "non-zero time iff non-zero count");
    return difference;
}

var numRecords = 100;

// check stats for specified key are as expected
var checked = {};
function checkStats(key, expected) {
    checked[key]++;
    var actual = diffTop(key).count;
    assert.eq(actual, expected, "top reports wrong count for " + key);
}

//  Insert
for (i = 0; i < numRecords; i++) {
    testColl.insert({_id: i});
}
checkStats("insert", numRecords);
checkStats("writeLock", numRecords);

// Update
for (i = 0; i < numRecords; i++) {
    testColl.update({_id: i}, {x: i});
}
checkStats("update", numRecords);

// Queries
var query = {};
for (i = 0; i < numRecords; i++) {
    query[i] = testColl.find({x: {$gte: i}}).batchSize(2);
    assert.eq(query[i].next()._id, i);
}
checkStats("queries", numRecords);

// Getmore
for (i = 0; i < numRecords / 2; i++) {
    assert.eq(query[i].next()._id, i + 1);
    assert.eq(query[i].next()._id, i + 2);
    assert.eq(query[i].next()._id, i + 3);
    assert.eq(query[i].next()._id, i + 4);
}
checkStats("getmore", numRecords);

// Remove
for (i = 0; i < numRecords; i++) {
    testColl.remove({_id: 1});
}
checkStats("remove", numRecords);

// Upsert, note that these are counted as updates, not inserts
for (i = 0; i < numRecords; i++) {
    testColl.update({_id: i}, {x: i}, {upsert: 1});
}
checkStats("update", numRecords);

// Commands
diffTop("commands");  // ignore any commands before this
for (i = 0; i < numRecords; i++) {
    assert.eq(testDB.runCommand({count: "toptestcoll"}).n, numRecords);
}
checkStats("commands", numRecords);

for (key in lastTop) {
    if (!(key in checked)) {
        printjson({key: key, stats: diffTop(key)});
    }
}
