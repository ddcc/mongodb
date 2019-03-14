//
// Utilities for testing chunk manipulation: moveChunk, mergeChunks, etc.
//

load('./jstests/libs/test_background_ops.js');

//
// Start a background moveChunk.
// staticMongod:   Server to use for communication, use
//                 "MongoRunner.runMongod({})" to make one.
// mongosURL:      Like 'localhost:27017'.
// findCriteria:   Like { _id: 1 }, passed to moveChunk's "find" option.
// bounds:         Array of two documents that specify the lower and upper
//                 shard key values of a chunk to move. Specify either the
//                 bounds field or the find field but not both.
// ns:             Like 'dbName.collectionName'.
// toShardId:      Like 'shard0001'.
//
// Returns a join function; call it to wait for moveChunk to complete.
//

function moveChunkParallel(staticMongod, mongosURL, findCriteria, bounds, ns, toShardId) {
    assert((findCriteria || bounds) && !(findCriteria && bounds),
           'Specify either findCriteria or bounds, but not both.');

    function runMoveChunk(mongosURL, findCriteria, bounds, ns, toShardId) {
        assert(mongosURL && ns && toShardId, 'Missing arguments.');
        assert((findCriteria || bounds) && !(findCriteria && bounds),
               'Specify either findCriteria or bounds, but not both.');

        var mongos = new Mongo(mongosURL), admin = mongos.getDB('admin'), cmd = {
            moveChunk: ns
        };

        if (findCriteria) {
            cmd.find = findCriteria;
        } else {
            cmd.bounds = bounds;
        }

        cmd.to = toShardId;
        cmd._waitForDelete = true;

        printjson(cmd);
        var result = admin.runCommand(cmd);
        printjson(result);
        assert(result.ok);
    }

    // Return the join function.
    return startParallelOps(
        staticMongod, runMoveChunk, [mongosURL, findCriteria, bounds, ns, toShardId]);
}

// moveChunk starts at step 0 and proceeds to 1 (it has *finished* parsing
// options), 2 (it has reloaded config and got distributed lock) and so on.
var moveChunkStepNames = {
    parsedOptions: 1,
    gotDistLock: 2,
    startedMoveChunk: 3,    // called _recvChunkStart on recipient
    reachedSteadyState: 4,  // recipient reports state is "steady"
    committed: 5,
    done: 6
};

function numberToName(names, stepNumber) {
    for (var name in names) {
        if (names.hasOwnProperty(name) && names[name] == stepNumber) {
            return name;
        }
    }

    assert(false);
}

//
// Configure a failpoint to make moveChunk hang at a step.
//
function pauseMoveChunkAtStep(shardConnection, stepNumber) {
    configureMoveChunkFailPoint(shardConnection, stepNumber, 'alwaysOn');
}

//
// Allow moveChunk to proceed past a step.
//
function unpauseMoveChunkAtStep(shardConnection, stepNumber) {
    configureMoveChunkFailPoint(shardConnection, stepNumber, 'off');
}

function proceedToMoveChunkStep(shardConnection, stepNumber) {
    jsTest.log('moveChunk proceeding from step "' +
               numberToName(moveChunkStepNames, stepNumber - 1) + '" to "' +
               numberToName(moveChunkStepNames, stepNumber) + '".');

    pauseMoveChunkAtStep(shardConnection, stepNumber);
    unpauseMoveChunkAtStep(shardConnection, stepNumber - 1);
    waitForMoveChunkStep(shardConnection, stepNumber);
}

function configureMoveChunkFailPoint(shardConnection, stepNumber, mode) {
    assert(stepNumber >= 1);
    assert(stepNumber <= 6);
    var admin = shardConnection.getDB('admin');
    assert.commandWorked(
        admin.runCommand({configureFailPoint: 'moveChunkHangAtStep' + stepNumber, mode: mode}));
}

//
// Wait for moveChunk to reach a step (1 through 6). Assumes only one moveChunk
// is in mongos's currentOp.
//
function waitForMoveChunkStep(shardConnection, stepNumber) {
    var searchString = 'step ' + stepNumber, admin = shardConnection.getDB('admin');

    assert(stepNumber >= 1);
    assert(stepNumber <= 6);

    var msg = ('moveChunk on ' + shardConnection.shardName + ' never reached step "' +
               numberToName(moveChunkStepNames, stepNumber) + '".');

    assert.soon(function() {
        var in_progress = admin.currentOp().inprog;
        for (var i = 0; i < in_progress.length; ++i) {
            var op = in_progress[i];
            if (op.query && op.query.moveChunk) {
                return op.msg && op.msg.startsWith(searchString);
            }
        }

        return false;
    }, msg);
}

var migrateStepNames = {
    copiedIndexes: 1,
    deletedPriorDataInRange: 2,
    cloned: 3,
    transferredMods: 4,  // About to enter steady state.
    done: 5
};

//
// Configure a failpoint to make migration thread hang at a step (1 through 5).
//
function pauseMigrateAtStep(shardConnection, stepNumber) {
    configureMigrateFailPoint(shardConnection, stepNumber, 'alwaysOn');
}

//
// Allow _recvChunkStart to proceed past a step.
//
function unpauseMigrateAtStep(shardConnection, stepNumber) {
    configureMigrateFailPoint(shardConnection, stepNumber, 'off');
}

function proceedToMigrateStep(shardConnection, stepNumber) {
    jsTest.log('Migration thread proceeding from step "' +
               numberToName(migrateStepNames, stepNumber - 1) + '" to "' +
               numberToName(migrateStepNames, stepNumber) + '".');

    pauseMigrateAtStep(shardConnection, stepNumber);
    unpauseMigrateAtStep(shardConnection, stepNumber - 1);
    waitForMigrateStep(shardConnection, stepNumber);
}

function configureMigrateFailPoint(shardConnection, stepNumber, mode) {
    assert(stepNumber >= 1);
    assert(stepNumber <= 5);
    var admin = shardConnection.getDB('admin');
    assert.commandWorked(admin.runCommand(
        {configureFailPoint: 'migrateThreadHangAtStep' + stepNumber, mode: mode}));
}

//
// Wait for moveChunk to reach a step (1 through 6).
//
function waitForMigrateStep(shardConnection, stepNumber) {
    var searchString = 'step ' + stepNumber, admin = shardConnection.getDB('admin');

    assert(stepNumber >= 1);
    assert(stepNumber <= 5);

    var msg = ('Migrate thread on ' + shardConnection.shardName + ' never reached step "' +
               numberToName(migrateStepNames, stepNumber) + '".');

    assert.soon(function() {
        // verbose = True so we see the migration thread.
        var in_progress = admin.currentOp(true).inprog;
        for (var i = 0; i < in_progress.length; ++i) {
            var op = in_progress[i];
            if (op.desc && op.desc === 'migrateThread') {
                if (op.hasOwnProperty('msg')) {
                    return op.msg.startsWith(searchString);
                } else {
                    return false;
                }
            }
        }

        return false;
    }, msg);
}
