'use strict';

/**
 * create_database.js
 *
 * Repeatedly creates and drops a database, with the focus on creation using different name casing.
 * Create using all different methods, implicitly by inserting, creating views/indexes etc.
 *
 * Each thread uses its own database, though sometimes threads may try to create databases with
 * names that only differ in case, expecting the appriopriate error code.
 */
var $config = (function() {

    var data = {
        checkCommandResult: function checkCommandResult(mayFailWithDatabaseDifferCase, res) {
            if (mayFailWithDatabaseDifferCase && !res.ok)
                assertAlways.commandFailedWithCode(res, ErrorCodes.DatabaseDifferCase);
            else
                assertAlways.commandWorked(res);
            return res;
        },

        checkWriteResult: function checkWriteResult(mayFailWithDatabaseDifferCase, res) {
            if (mayFailWithDatabaseDifferCase && res.hasWriteError()) {
                assertAlways.eq(res.getWriteError().code, ErrorCodes.DatabaseDifferCase);
            } else
                assertAlways.writeOK(res);
            return res;
        }
    };

    var states = (function() {
        function init(db, collName) {
            var uniqueNr = this.tid;
            var semiUniqueNr = Math.floor(uniqueNr / 2);

            // The semiUniqueDBName may clash and result in a DatabaseDifferCas error on
            // creation,
            // while the uniqueDBName does not clash. The unique and created variables track
            // this.
            this.semiUniqueDBName =
                (this.tid % 2 ? 'create_database' : 'CREATE_DATABASE') + semiUniqueNr;
            this.uniqueDBName = 'CreateDatabase' + uniqueNr;
            this.myDB = db.getSiblingDB(this.uniqueDBName);
            this.created = false;
            this.unique = true;
        }

        function useSemiUniqueDBName(db, collName) {
            this.myDB = db.getSiblingDB(this.semiUniqueDBName);
            this.unique = false;
        }

        function createCollection(db, collName) {
            this.created =
                this.checkCommandResult(!this.unique, this.myDB.createCollection(collName)).ok;
        }

        function createIndex(db, collName) {
            var background = Math.random > 0.5;
            var res = this.myDB.getCollection(collName).createIndex({x: 1}, {background});
            this.created |=
                this.checkCommandResult(!this.unique, res).createdCollectionAutomatically;
        }

        function insert(db, collName) {
            this.created |= this.checkWriteResult(!this.created && !this.unique,
                                                  this.myDB.getCollection(collName).insert({x: 1}))
                                .nInserted == 1;
        }

        function upsert(db, collName) {
            this.created |= this.checkWriteResult(!this.created && !this.unique,
                                                  this.myDB.getCollection(collName).update(
                                                      {x: 1}, {x: 2}, {upsert: 1})).nUpserted == 1;
        }

        function drop(db, collName) {
            if (this.created)
                assertAlways(this.myDB.getCollection(collName).drop());
        }

        function dropDatabase(db, collName) {
            if (this.created)
                assertAlways.commandWorked(this.myDB.dropDatabase());
        }

        function listDatabases(db, collName) {
            for (var database of db.adminCommand({listDatabases: 1}).databases) {
                var res = db.getSiblingDB(database.name).runCommand({listCollections: 1});
                assertAlways.commandWorked(res);
                assertAlways.neq(database.name, this.myDB.toString(), "this DB shouldn't exist");
            }
        }

        function listDatabasesNameOnly(db, collName) {
            for (var database of db.adminCommand({listDatabases: 1, nameOnly: 1}).databases) {
                var res = db.getSiblingDB(database.name).runCommand({listCollections: 1});
                assertAlways.commandWorked(res);
                assertAlways.neq(database.name, this.myDB.toString(), "this DB shouldn't exist");
            }
        }

        return {
            init: init,
            useSemiUniqueDBName: useSemiUniqueDBName,
            createCollection: createCollection,
            createIndex: createIndex,
            insert: insert,
            upsert: upsert,
            drop: drop,
            dropDatabase: dropDatabase,
            listDatabases: listDatabases,
            listDatabasesNameOnly: listDatabasesNameOnly,
        };
    })();

    var transitions = {
        init: {
            useSemiUniqueDBName: 0.25,
            createCollection: 0.375,
            createIndex: 0.125,
            insert: 0.125,
            upsert: 0.125
        },
        useSemiUniqueDBName: {createCollection: 1.00},
        createCollection: {dropDatabase: 0.25, createIndex: 0.25, insert: 0.25, upsert: 0.25},
        createIndex: {insert: 0.25, upsert: 0.25, dropDatabase: 0.5},
        insert: {dropDatabase: 0.2, drop: 0.05, insert: 0.5, upsert: 0.25},
        upsert: {dropDatabase: 0.2, drop: 0.05, insert: 0.25, upsert: 0.5},
        drop: {dropDatabase: 0.75, init: 0.25},  // OK to leave the empty database behind sometimes
        dropDatabase: {init: 0.75, listDatabases: 0.15, listDatabasesNameOnly: 0.10},
        listDatabases: {init: 0.75, listDatabases: 0.15, listDatabasesNameOnly: 0.10},
        listDatabasesNameOnly: {init: 0.75, listDatabases: 0.10, listDatabasesNameOnly: 0.15},
    };

    return {
        data: data,
        // We only run a few iterations to reduce the amount of data cumulatively
        // written to disk by mmapv1. For example, setting 10 threads and 180
        // iterations (with an expected 6 transitions per create/drop roundtrip)
        // causes this workload to write at least 32MB (.ns and .0 files) * 10 threads
        // * 30 iterations worth of data to disk, or about 10GB, which can be slow on
        // test hosts.
        threadCount: 10,
        iterations: 180,
        states: states,
        transitions: transitions,
    };
})();
