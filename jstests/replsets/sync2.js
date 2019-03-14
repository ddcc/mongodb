// Tests that nodes sync from each other properly and that nodes find new sync sources when they
// are disconnected from their current sync source.

(function() {
    'use strict';

    var replTest = new ReplSetTest({
        name: 'sync2',
        nodes: [{rsConfig: {priority: 5}}, {arbiter: true}, {}, {}, {}],
        useBridge: true
    });
    var conns = replTest.startSet();
    replTest.initiate();

    var master = replTest.getPrimary();
    jsTestLog("Replica set test initialized");

    master.getDB("foo").bar.insert({x: 1});
    replTest.awaitReplication();

    conns[0].disconnect(conns[4]);
    conns[1].disconnect(conns[2]);
    conns[2].disconnect(conns[3]);
    conns[3].disconnect(conns[1]);

    // 4 is connected to 2
    conns[4].disconnect(conns[1]);
    conns[4].disconnect(conns[3]);

    assert.soon(function() {
        master = replTest.getPrimary();
        return master === conns[0];
    }, replTest.kDefaultTimeoutMS, "node 0 did not become primary quickly enough");

    replTest.awaitReplication();
    jsTestLog("Checking that ops still replicate correctly");
    var option = {
        writeConcern: {w: conns.length - 1, wtimeout: replTest.kDefaultTimeoutMS}
    };
    // In PV0, this write can fail as a result of a bad spanning tree. If 2 was syncing from 4 prior
    // to bridging, it will not change sync sources and receive the write in time. This was not a
    // problem in 3.0 because the old version of mongobridge caused all the nodes to restart during
    // partitioning, forcing the set to rebuild the spanning tree.
    assert.writeOK(master.getDB("foo").bar.insert({x: 1}, option));

    // 4 is connected to 3
    conns[4].disconnect(conns[2]);
    conns[4].reconnect(conns[3]);

    assert.writeOK(master.getDB("foo").bar.insert({x: 1}, option));

    replTest.stopSet();
}());
