// check notablescan mode
//
// @tags: [
//   # This test attempts to perform read operations after having enabled the notablescan server
//   # parameter. The former operations may be routed to a secondary in the replica set, whereas the
//   # latter must be routed to the primary.
//   assumes_read_preference_unchanged,
// ]

t = db.test_notablescan;
t.drop();

try {
    assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: true}));
    // commented lines are SERVER-2222
    if (0) {  // SERVER-2222
        assert.throws(function() {
            t.find({a: 1}).toArray();
        });
    }
    t.save({a: 1});
    if (0) {  // SERVER-2222
        assert.throws(function() {
            t.count({a: 1});
        });
        assert.throws(function() {
            t.find({}).toArray();
        });
    }
    assert.eq(1, t.find({}).itcount());  // SERVER-274
    assert.throws(function() {
        t.find({a: 1}).toArray();
    });
    assert.throws(function() {
        t.find({a: 1}).hint({$natural: 1}).toArray();
    });
    t.ensureIndex({a: 1});
    assert.eq(0, t.find({a: 1, b: 1}).itcount());
    assert.eq(1, t.find({a: 1, b: null}).itcount());

    // SERVER-4327
    assert.eq(0, t.find({a: {$in: []}}).itcount());
    assert.eq(0, t.find({a: {$in: []}, b: 0}).itcount());
} finally {
    // We assume notablescan was false before this test started and restore that
    // expected value.
    assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: false}));
}
