// SERVER-24963/SERVER-27930 Missing invalidation for system.indexes writes
(function() {
    'use strict';
    var collName = 'system_indexes_invalidations';
    var collNameRenamed = 'renamed_collection';
    var coll = db[collName];
    var collRenamed = db[collNameRenamed];

    function testIndexInvalidation(isRename) {
        coll.drop();
        collRenamed.drop();
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {c: 1}]));

        // Get the first two indexes. Use find on 'system.indexes' on MMAPv1, listIndexes otherwise.
        var cmd = db.system.indexes.count() ? {find: 'system.indexes'} : {listIndexes: collName};
        Object.extend(cmd, {batchSize: 2});
        var res = db.runCommand(cmd);
        assert.commandWorked(res, 'could not run ' + tojson(cmd));
        printjson(res);

        // Ensure the cursor has data, rename or drop the collection, and exhaust the cursor.
        var cursor = new DBCommandCursor(db.getMongo(), res);
        var errMsg =
            'expected more data from command ' + tojson(cmd) + ', with result ' + tojson(res);
        assert(cursor.hasNext(), errMsg);
        if (isRename) {
            assert.commandWorked(coll.renameCollection(collNameRenamed));
        } else {
            assert(coll.drop());
        }
        assert.gt(cursor.itcount(), 0, errMsg);
    }

    // Test that we invalidate indexes for both collection drops and renames.
    testIndexInvalidation(false);
    testIndexInvalidation(true);
}());
