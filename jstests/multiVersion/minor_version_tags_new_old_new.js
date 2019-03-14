(function() {
    'use strict';

    load("jstests/replsets/libs/tags.js");

    // 3.2.1 is the final version to use the old style replSetUpdatePosition command.
    var oldVersion = "3.2.1";
    var newVersion = "latest";
    var nodes = [
        {binVersion: newVersion},
        {binVersion: oldVersion},
        {binVersion: newVersion},
        {binVersion: oldVersion},
        {binVersion: newVersion}
    ];
    new TagsTest({nodes: nodes, forceWriteMode: 'commands'}).run();
}());
