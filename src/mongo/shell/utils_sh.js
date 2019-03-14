sh = function() {
    return "try sh.help();";
};

sh._checkMongos = function() {
    var x = db.runCommand("ismaster");
    if (x.msg != "isdbgrid")
        throw Error("not connected to a mongos");
};

sh._checkFullName = function(fullName) {
    assert(fullName, "need a full name");
    assert(fullName.indexOf(".") > 0, "name needs to be fully qualified <db>.<collection>'");
};

sh._adminCommand = function(cmd, skipCheck) {
    if (!skipCheck)
        sh._checkMongos();
    return db.getSisterDB("admin").runCommand(cmd);
};

sh._getConfigDB = function() {
    sh._checkMongos();
    return db.getSiblingDB("config");
};

sh._dataFormat = function(bytes) {
    if (bytes < 1024)
        return Math.floor(bytes) + "B";
    if (bytes < 1024 * 1024)
        return Math.floor(bytes / 1024) + "KiB";
    if (bytes < 1024 * 1024 * 1024)
        return Math.floor((Math.floor(bytes / 1024) / 1024) * 100) / 100 + "MiB";
    return Math.floor((Math.floor(bytes / (1024 * 1024)) / 1024) * 100) / 100 + "GiB";
};

sh._collRE = function(coll) {
    return RegExp("^" + RegExp.escape(coll + "") + "-.*");
};

sh._pchunk = function(chunk) {
    return "[" + tojson(chunk.min) + " -> " + tojson(chunk.max) + "]";
};

sh.help = function() {
    print("\tsh.addShard( host )                       server:port OR setname/server:port");
    print("\tsh.enableSharding(dbname)                 enables sharding on the database dbname");
    print("\tsh.shardCollection(fullName,key,unique)   shards the collection");

    print(
        "\tsh.splitFind(fullName,find)               splits the chunk that find is in at the median");
    print(
        "\tsh.splitAt(fullName,middle)               splits the chunk that middle is in at middle");
    print(
        "\tsh.moveChunk(fullName,find,to)            move the chunk where 'find' is to 'to' (name of shard)");

    print(
        "\tsh.setBalancerState( <bool on or not> )   turns the balancer on or off true=on, false=off");
    print("\tsh.getBalancerState()                     return true if enabled");
    print(
        "\tsh.isBalancerRunning()                    return true if the balancer has work in progress on any mongos");

    print("\tsh.disableBalancing(coll)                 disable balancing on one collection");
    print("\tsh.enableBalancing(coll)                  re-enable balancing on one collection");

    print("\tsh.addShardTag(shard,tag)                 adds the tag to the shard");
    print("\tsh.removeShardTag(shard,tag)              removes the tag from the shard");
    print(
        "\tsh.addTagRange(fullName,min,max,tag)      tags the specified range of the given collection");
    print(
        "\tsh.removeTagRange(fullName,min,max,tag)   removes the tagged range of the given collection");

    print("\tsh.status()                               prints a general overview of the cluster");
};

sh.status = function(verbose, configDB) {
    // TODO: move the actual command here
    printShardingStatus(configDB, verbose);
};

sh.addShard = function(url) {
    return sh._adminCommand({addShard: url}, true);
};

sh.enableSharding = function(dbname) {
    assert(dbname, "need a valid dbname");
    return sh._adminCommand({enableSharding: dbname});
};

sh.shardCollection = function(fullName, key, unique) {
    sh._checkFullName(fullName);
    assert(key, "need a key");
    assert(typeof(key) == "object", "key needs to be an object");

    var cmd = {
        shardCollection: fullName,
        key: key
    };
    if (unique)
        cmd.unique = true;

    return sh._adminCommand(cmd);
};

sh.splitFind = function(fullName, find) {
    sh._checkFullName(fullName);
    return sh._adminCommand({split: fullName, find: find});
};

sh.splitAt = function(fullName, middle) {
    sh._checkFullName(fullName);
    return sh._adminCommand({split: fullName, middle: middle});
};

sh.moveChunk = function(fullName, find, to) {
    sh._checkFullName(fullName);
    return sh._adminCommand({moveChunk: fullName, find: find, to: to});
};

sh.setBalancerState = function(onOrNot) {
    return assert.writeOK(sh._getConfigDB().settings.update(
        {_id: 'balancer'},
        {$set: {stopped: onOrNot ? false : true}},
        {upsert: true, writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.getBalancerState = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    var x = configDB.settings.findOne({_id: "balancer"});
    if (x == null)
        return true;
    return !x.stopped;
};

sh.isBalancerRunning = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    var x = configDB.locks.findOne({_id: "balancer"});
    if (x == null) {
        print("config.locks collection empty or missing. be sure you are connected to a mongos");
        return false;
    }
    return x.state > 0;
};

sh.getBalancerHost = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    var x = configDB.locks.findOne({_id: "balancer"});
    if (x == null) {
        print(
            "config.locks collection does not contain balancer lock. be sure you are connected to a mongos");
        return "";
    }
    return x.process.match(/[^:]+:[^:]+/)[0];
};

sh.stopBalancer = function(timeout, interval) {
    var res = sh.setBalancerState(false);
    sh.waitForBalancer(false, timeout, interval);
    return res;
};

sh.startBalancer = function(timeout, interval) {
    var res = sh.setBalancerState(true);
    sh.waitForBalancer(true, timeout, interval);
    return res;
};

sh.waitForDLock = function(lockId, onOrNot, timeout, interval) {
    // Wait for balancer to be on or off
    // Can also wait for particular balancer state
    var state = onOrNot;
    var configDB = sh._getConfigDB();

    var beginTS = undefined;
    if (state == undefined) {
        var currLock = configDB.locks.findOne({_id: lockId});
        if (currLock != null)
            beginTS = currLock.ts;
    }

    var lockStateOk = function() {
        var lock = configDB.locks.findOne({_id: lockId});

        if (state == false)
            return !lock || lock.state == 0;
        if (state == true)
            return lock && lock.state == 2;
        if (state == undefined)
            return (beginTS == undefined && lock) ||
                (beginTS != undefined && (!lock || lock.ts + "" != beginTS + ""));
        else
            return lock && lock.state == state;
    };

    assert.soon(
        lockStateOk,
        "Waited too long for lock " + lockId + " to " +
            (state == true ? "lock" : (state == false ? "unlock" : "change to state " + state)),
        timeout,
        interval);
};

sh.waitForPingChange = function(activePings, timeout, interval) {

    var isPingChanged = function(activePing) {
        var newPing = sh._getConfigDB().mongos.findOne({_id: activePing._id});
        return !newPing || newPing.ping + "" != activePing.ping + "";
    };

    // First wait for all active pings to change, so we're sure a settings reload
    // happened

    // Timeout all pings on the same clock
    var start = new Date();

    var remainingPings = [];
    for (var i = 0; i < activePings.length; i++) {
        var activePing = activePings[i];
        print("Waiting for active host " + activePing._id +
              " to recognize new settings... (ping : " + activePing.ping + ")");

        // Do a manual timeout here, avoid scary assert.soon errors
        var timeout = timeout || 30000;
        var interval = interval || 200;
        while (isPingChanged(activePing) != true) {
            if ((new Date()).getTime() - start.getTime() > timeout) {
                print("Waited for active ping to change for host " + activePing._id +
                      ", a migration may be in progress or the host may be down.");
                remainingPings.push(activePing);
                break;
            }
            sleep(interval);
        }
    }

    return remainingPings;
};

sh.waitForBalancerOff = function(timeout, interval) {
    var pings = sh._getConfigDB().mongos.find().toArray();
    var activePings = [];
    for (var i = 0; i < pings.length; i++) {
        if (!pings[i].waiting)
            activePings.push(pings[i]);
    }

    print("Waiting for active hosts...");

    activePings = sh.waitForPingChange(activePings, 60 * 1000);

    // After 1min, we assume that all hosts with unchanged pings are either
    // offline (this is enough time for a full errored balance round, if a network
    // issue, which would reload settings) or balancing, which we wait for next
    // Legacy hosts we always have to wait for

    print("Waiting for the balancer lock...");

    // Wait for the balancer lock to become inactive
    // We can guess this is stale after 15 mins, but need to double-check manually
    try {
        sh.waitForDLock("balancer", false, 15 * 60 * 1000);
    } catch (e) {
        print(
            "Balancer still may be active, you must manually verify this is not the case using the config.changelog collection.");
        throw Error(e);
    }

    print("Waiting again for active hosts after balancer is off...");

    // Wait a short time afterwards, to catch the host which was balancing earlier
    activePings = sh.waitForPingChange(activePings, 5 * 1000);

    // Warn about all the stale host pings remaining
    for (var i = 0; i < activePings.length; i++) {
        print("Warning : host " + activePings[i]._id + " seems to have been offline since " +
              activePings[i].ping);
    }

};

sh.waitForBalancer = function(onOrNot, timeout, interval) {

    // If we're waiting for the balancer to turn on or switch state or
    // go to a particular state
    if (onOrNot) {
        // Just wait for the balancer lock to change, can't ensure we'll ever see it
        // actually locked
        sh.waitForDLock("balancer", undefined, timeout, interval);
    } else {
        // Otherwise we need to wait until we're sure balancing stops
        sh.waitForBalancerOff(timeout, interval);
    }

};

sh.disableBalancing = function(coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    var dbase = db;
    if (coll instanceof DBCollection) {
        dbase = coll.getDB();
    } else {
        sh._checkMongos();
    }

    return assert.writeOK(dbase.getSisterDB("config").collections.update(
        {_id: coll + ""},
        {$set: {"noBalance": true}},
        {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.enableBalancing = function(coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    var dbase = db;
    if (coll instanceof DBCollection) {
        dbase = coll.getDB();
    } else {
        sh._checkMongos();
    }

    return assert.writeOK(dbase.getSisterDB("config").collections.update(
        {_id: coll + ""},
        {$set: {"noBalance": false}},
        {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

/*
 * Can call _lastMigration( coll ), _lastMigration( db ), _lastMigration( st ), _lastMigration(
 * mongos )
 */
sh._lastMigration = function(ns) {

    var coll = null;
    var dbase = null;
    var config = null;

    if (!ns) {
        config = db.getSisterDB("config");
    } else if (ns instanceof DBCollection) {
        coll = ns;
        config = coll.getDB().getSisterDB("config");
    } else if (ns instanceof DB) {
        dbase = ns;
        config = dbase.getSisterDB("config");
    } else if (ns instanceof ShardingTest) {
        config = ns.s.getDB("config");
    } else if (ns instanceof Mongo) {
        config = ns.getDB("config");
    } else {
        // String namespace
        ns = ns + "";
        if (ns.indexOf(".") > 0) {
            config = db.getSisterDB("config");
            coll = db.getMongo().getCollection(ns);
        } else {
            config = db.getSisterDB("config");
            dbase = db.getSisterDB(ns);
        }
    }

    var searchDoc = {
        what: /^moveChunk/
    };
    if (coll)
        searchDoc.ns = coll + "";
    if (dbase)
        searchDoc.ns = new RegExp("^" + dbase + "\\.");

    var cursor = config.changelog.find(searchDoc).sort({time: -1}).limit(1);
    if (cursor.hasNext())
        return cursor.next();
    else
        return null;
};

sh.addShardTag = function(shard, tag) {
    var config = sh._getConfigDB();
    if (config.shards.findOne({_id: shard}) == null) {
        throw Error("can't find a shard with name: " + shard);
    }
    return assert.writeOK(config.shards.update(
        {_id: shard}, {$addToSet: {tags: tag}}, {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.removeShardTag = function(shard, tag) {
    var config = sh._getConfigDB();
    if (config.shards.findOne({_id: shard}) == null) {
        throw Error("can't find a shard with name: " + shard);
    }
    return assert.writeOK(config.shards.update(
        {_id: shard}, {$pull: {tags: tag}}, {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.addTagRange = function(ns, min, max, tag) {
    if (bsonWoCompare(min, max) == 0) {
        throw new Error("min and max cannot be the same");
    }

    var config = sh._getConfigDB();
    return assert.writeOK(
        config.tags.update({_id: {ns: ns, min: min}},
                           {_id: {ns: ns, min: min}, ns: ns, min: min, max: max, tag: tag},
                           {upsert: true, writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.removeTagRange = function(ns, min, max, tag) {
    var config = sh._getConfigDB();
    // warn if the namespace does not exist, even dropped
    if (config.collections.findOne({_id: ns}) == null) {
        print("Warning: can't find the namespace: " + ns + " - collection likely never sharded");
    }
    // warn if the tag being removed is still in use
    if (config.shards.findOne({tags: tag})) {
        print("Warning: tag still in use by at least one shard");
    }
    // max and tag criteria not really needed, but including them avoids potentially unexpected
    // behavior.
    return assert.writeOK(config.tags.remove({_id: {ns: ns, min: min}, max: max, tag: tag},
                                             {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.getBalancerLockDetails = function(configDB) {
    if (configDB === undefined)
        configDB = db.getSiblingDB('config');
    var lock = configDB.locks.findOne({_id: 'balancer'});
    if (lock == null) {
        return null;
    }
    if (lock.state == 0) {
        return null;
    }
    return lock;
};

sh.getBalancerWindow = function(configDB) {
    if (configDB === undefined)
        configDB = db.getSiblingDB('config');
    var settings = configDB.settings.findOne({_id: 'balancer'});
    if (settings == null) {
        return null;
    }
    if (settings.hasOwnProperty("activeWindow")) {
        return settings.activeWindow;
    }
    return null;
};

sh.getActiveMigrations = function(configDB) {
    if (configDB === undefined)
        configDB = db.getSiblingDB('config');
    var activeLocks = configDB.locks.find({_id: {$ne: "balancer"}, state: {$eq: 2}});
    var result = [];
    if (activeLocks != null) {
        activeLocks.forEach(function(lock) {
            result.push({_id: lock._id, when: lock.when});
        });
    }
    return result;
};

sh.getRecentFailedRounds = function(configDB) {
    if (configDB === undefined)
        configDB = db.getSiblingDB('config');
    var balErrs = configDB.actionlog.find({what: "balancer.round"}).sort({time: -1}).limit(5);
    var result = {
        count: 0,
        lastErr: "",
        lastTime: " "
    };
    if (balErrs != null) {
        balErrs.forEach(function(r) {
            if (r.details.errorOccured) {
                result.count += 1;
                result.lastErr = r.details.errmsg;
                result.lastTime = r.time;
            }
        });
    }
    return result;
};

/**
 * Returns a summary of chunk migrations that was completed either successfully or not
 * since yesterday. The format is an array of 2 arrays, where the first array contains
 * the successful cases, and the second array contains the failure cases.
 */
sh.getRecentMigrations = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    var yesterday = new Date(new Date() - 24 * 60 * 60 * 1000);

    // Successful migrations.
    var result = configDB.changelog.aggregate([
        {
          $match: {
              time: {$gt: yesterday},
              what: "moveChunk.from", 'details.errmsg': {$exists: false}, 'details.note': 'success'
          }
        },
        {$group: {_id: {msg: "$details.errmsg"}, count: {$sum: 1}}},
        {$project: {_id: {$ifNull: ["$_id.msg", "Success"]}, count: "$count"}}
    ]).toArray();

    // Failed migrations.
    result = result.concat(configDB.changelog.aggregate([
        {
          $match: {
              time: {$gt: yesterday},
              what: "moveChunk.from",
              $or: [{'details.errmsg': {$exists: true}}, {'details.note': {$ne: 'success'}}]
          }
        },
        {
          $group: {
              _id: {msg: "$details.errmsg", from: "$details.from", to: "$details.to"},
              count: {$sum: 1}
          }
        },
        {
          $project: {
              _id: {$ifNull: ['$_id.msg', 'aborted']},
              from: "$_id.from",
              to: "$_id.to",
              count: "$count"
          }
        }
    ]).toArray());

    return result;
};

sh._shardingStatusStr = function(indent, s) {
    // convert from logical indentation to actual num of chars
    if (indent == 0) {
        indent = 0;
    } else if (indent == 1) {
        indent = 2;
    } else {
        indent = (indent - 1) * 8;
    }
    return indentStr(indent, s) + "\n";
};

function printShardingStatus(configDB, verbose) {
    // configDB is a DB object that contains the sharding metadata of interest.
    // Defaults to the db named "config" on the current connection.
    if (configDB === undefined)
        configDB = db.getSisterDB('config');

    var version = configDB.getCollection("version").findOne();
    if (version == null) {
        print(
            "printShardingStatus: this db does not have sharding enabled. be sure you are connecting to a mongos from the shell and not to a mongod.");
        return;
    }

    var raw = "";
    var output = function(indent, s) {
        raw += sh._shardingStatusStr(indent, s);
    };
    output(0, "--- Sharding Status --- ");
    output(1, "sharding version: " + tojson(configDB.getCollection("version").findOne()));

    output(1, "shards:");
    configDB.shards.find().sort({_id: 1}).forEach(function(z) {
        output(2, tojsononeline(z));
    });

    // (most recently) active mongoses
    var mongosActiveThresholdMs = 60000;
    var mostRecentMongos = configDB.mongos.find().sort({ping: -1}).limit(1);
    var mostRecentMongosTime = null;
    var mongosAdjective = "most recently active";
    if (mostRecentMongos.hasNext()) {
        mostRecentMongosTime = mostRecentMongos.next().ping;
        // Mongoses older than the threshold are the most recent, but cannot be
        // considered "active" mongoses. (This is more likely to be an old(er)
        // configdb dump, or all the mongoses have been stopped.)
        if (mostRecentMongosTime.getTime() >= Date.now() - mongosActiveThresholdMs) {
            mongosAdjective = "active";
        }
    }

    output(1, mongosAdjective + " mongoses:");
    if (mostRecentMongosTime === null) {
        output(2, "none");
    } else {
        var recentMongosQuery = {
            ping: {
                $gt: (function() {
                    var d = mostRecentMongosTime;
                    d.setTime(d.getTime() - mongosActiveThresholdMs);
                    return d;
                })()
            }
        };

        if (verbose) {
            configDB.mongos.find(recentMongosQuery)
                .sort({ping: -1})
                .forEach(function(z) {
                    output(2, tojsononeline(z));
                });
        } else {
            configDB.mongos.aggregate([
                {$match: recentMongosQuery},
                {$group: {_id: "$mongoVersion", num: {$sum: 1}}},
                {$sort: {num: -1}}
            ])
                .forEach(function(z) {
                    output(2, tojson(z._id) + " : " + z.num);
                });
        }
    }

    output(1, "balancer:");

    // Is the balancer currently enabled
    output(2, "Currently enabled:  " + (sh.getBalancerState(configDB) ? "yes" : "no"));

    // Is the balancer currently active
    output(2, "Currently running:  " + (sh.isBalancerRunning(configDB) ? "yes" : "no"));

    // Output details of the current balancer round
    var balLock = sh.getBalancerLockDetails(configDB);
    if (balLock) {
        output("\t\tBalancer lock taken at " + balLock.when + " by " + balLock.who);
    }

    // Output the balancer window
    var balSettings = sh.getBalancerWindow(configDB);
    if (balSettings) {
        output(3,
               "Balancer active window is set between " + balSettings.start + " and " +
                   balSettings.stop + " server local time");
    }

    // Output the list of active migrations
    var activeMigrations = sh.getActiveMigrations(configDB);
    if (activeMigrations.length > 0) {
        output(2, "Collections with active migrations: ");
        activeMigrations.forEach(function(migration) {
            output(3, migration._id + " started at " + migration.when);
        });
    }

    // Actionlog and version checking only works on 2.7 and greater
    var versionHasActionlog = false;
    var metaDataVersion = configDB.getCollection("version").findOne().currentVersion;
    if (metaDataVersion > 5) {
        versionHasActionlog = true;
    }
    if (metaDataVersion == 5) {
        var verArray = db.serverBuildInfo().versionArray;
        if (verArray[0] == 2 && verArray[1] > 6) {
            versionHasActionlog = true;
        }
    }

    if (versionHasActionlog) {
        // Review config.actionlog for errors
        var actionReport = sh.getRecentFailedRounds(configDB);
        // Always print the number of failed rounds
        output(2, "Failed balancer rounds in last 5 attempts:  " + actionReport.count);

        // Only print the errors if there are any
        if (actionReport.count > 0) {
            output(2, "Last reported error:  " + actionReport.lastErr);
            output(2, "Time of Reported error:  " + actionReport.lastTime);
        }

        output(2, "Migration Results for the last 24 hours: ");
        var migrations = sh.getRecentMigrations(configDB);
        if (migrations.length > 0) {
            migrations.forEach(function(x) {
                if (x._id === "Success") {
                    output(3, x.count + " : " + x._id);
                } else {
                    output(3,
                           x.count + " : Failed with error '" + x._id + "', from " + x.from +
                               " to " + x.to);
                }
            });
        } else {
            output(3, "No recent migrations");
        }
    }

    output(1, "databases:");
    configDB.databases.find().sort({name: 1}).forEach(function(db) {
        var truthy = function(value) {
            return !!value;
        };
        var nonBooleanNote = function(name, value) {
            // If the given value is not a boolean, return a string of the
            // form " (<name>: <value>)", where <value> is converted to JSON.
            var t = typeof(value);
            var s = "";
            if (t != "boolean" && t != "undefined") {
                s = " (" + name + ": " + tojson(value) + ")";
            }
            return s;
        };

        output(2, tojsononeline(db, "", true));

        if (db.partitioned) {
            configDB.collections.find({_id: new RegExp("^" + RegExp.escape(db._id) + "\\.")})
                .sort({_id: 1})
                .forEach(function(coll) {
                    if (!coll.dropped) {
                        output(3, coll._id);
                        output(4, "shard key: " + tojson(coll.key));
                        output(4,
                               "unique: " + truthy(coll.unique) +
                                   nonBooleanNote("unique", coll.unique));
                        output(4,
                               "balancing: " + !truthy(coll.noBalance) +
                                   nonBooleanNote("noBalance", coll.noBalance));
                        output(4, "chunks:");

                        res = configDB.chunks
                                  .aggregate({$match: {ns: coll._id}},
                                             {$group: {_id: "$shard", cnt: {$sum: 1}}},
                                             {$project: {_id: 0, shard: "$_id", nChunks: "$cnt"}},
                                             {$sort: {shard: 1}})
                                  .toArray();
                        var totalChunks = 0;
                        res.forEach(function(z) {
                            totalChunks += z.nChunks;
                            output(5, z.shard + "\t" + z.nChunks);
                        });

                        if (totalChunks < 20 || verbose) {
                            configDB.chunks.find({"ns": coll._id})
                                .sort({min: 1})
                                .forEach(function(chunk) {
                                    output(4,
                                           tojson(chunk.min) + " -->> " + tojson(chunk.max) +
                                               " on : " + chunk.shard + " " +
                                               tojson(chunk.lastmod) + " " +
                                               (chunk.jumbo ? "jumbo " : ""));
                                });
                        } else {
                            output(
                                4,
                                "too many chunks to print, use verbose if you want to force print");
                        }

                        configDB.tags.find({ns: coll._id})
                            .sort({min: 1})
                            .forEach(function(tag) {
                                output(4,
                                       " tag: " + tag.tag + "  " + tojson(tag.min) + " -->> " +
                                           tojson(tag.max));
                            });
                    }
                });
        }
    });

    print(raw);
}

function printShardingSizes(configDB) {
    // configDB is a DB object that contains the sharding metadata of interest.
    // Defaults to the db named "config" on the current connection.
    if (configDB === undefined)
        configDB = db.getSisterDB('config');

    var version = configDB.getCollection("version").findOne();
    if (version == null) {
        print("printShardingSizes : not a shard db!");
        return;
    }

    var raw = "";
    var output = function(indent, s) {
        raw += sh._shardingStatusStr(indent, s);
    };
    output(0, "--- Sharding Sizes --- ");
    output(1, "sharding version: " + tojson(configDB.getCollection("version").findOne()));

    output(1, "shards:");
    var shards = {};
    configDB.shards.find().forEach(function(z) {
        shards[z._id] = new Mongo(z.host);
        output(2, tojson(z));
    });

    var saveDB = db;
    output(1, "databases:");
    configDB.databases.find().sort({name: 1}).forEach(function(db) {
        output(2, tojson(db, "", true));

        if (db.partitioned) {
            configDB.collections.find({_id: new RegExp("^" + RegExp.escape(db._id) + "\.")})
                .sort({_id: 1})
                .forEach(function(coll) {
                    output(3, coll._id + " chunks:");
                    configDB.chunks.find({"ns": coll._id})
                        .sort({min: 1})
                        .forEach(function(chunk) {
                            var mydb = shards[chunk.shard].getDB(db._id);
                            var out = mydb.runCommand({
                                dataSize: coll._id,
                                keyPattern: coll.key,
                                min: chunk.min,
                                max: chunk.max
                            });
                            delete out.millis;
                            delete out.ok;

                            output(4,
                                   tojson(chunk.min) + " -->> " + tojson(chunk.max) + " on : " +
                                       chunk.shard + " " + tojson(out));

                        });
                });
        }
    });

    print(raw);
}
