"""
Interface of the different fixtures for executing JSTests against.
"""

from __future__ import absolute_import

import time

import pymongo

from ... import errors
from ... import logging


class Fixture(object):
    """
    Base class for all fixtures.
    """

    def __init__(self, logger, job_num):
        """
        Initializes the fixtures with a logger instance.
        """

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        if not isinstance(job_num, int):
            raise TypeError("job_num must be an integer")
        elif job_num < 0:
            raise ValueError("job_num must be a nonnegative integer")

        self.logger = logger
        self.job_num = job_num

        self.port = None  # Port that the mongo shell should connect to.

    def setup(self):
        """
        Creates the fixture.
        """
        pass

    def await_ready(self):
        """
        Blocks until the fixture can be used for testing.
        """
        pass

    def teardown(self):
        """
        Destroys the fixture. Return true if was successful, and false otherwise.
        """
        return True

    def is_running(self):
        """
        Returns true if the fixture is still operating and more tests
        can be run, and false otherwise.
        """
        return True

    def get_internal_connection_string(self):
        """
        Returns the connection string for this fixture. This is NOT a
        driver connection string, but a connection string of the format
        expected by the mongo::ConnectionString class.
        """
        raise NotImplementedError(
            "get_internal_connection_string must be implemented by Fixture subclasses")

    def get_driver_connection_url(self):
        """
        Return the mongodb connection string as defined here:
        https://docs.mongodb.com/manual/reference/connection-string/
        """
        raise NotImplementedError(
            "get_driver_connection_url must be implemented by Fixture subclasses")

    def __str__(self):
        return "%s (Job #%d)" % (self.__class__.__name__, self.job_num)

    def __repr__(self):
        return "%r(%r, %r)" % (self.__class__.__name__, self.logger, self.job_num)


class ReplFixture(Fixture):
    """
    Base class for all fixtures that support replication.
    """

    AWAIT_REPL_TIMEOUT_MINS = 5

    def get_primary(self):
        """
        Returns the primary of a replica set, or the master of a
        master-slave deployment.
        """
        raise NotImplementedError("get_primary must be implemented by ReplFixture subclasses")

    def get_secondaries(self):
        """
        Returns a list containing the secondaries of a replica set, or
        the slave of a master-slave deployment.
        """
        raise NotImplementedError("get_secondaries must be implemented by ReplFixture subclasses")

    def retry_until_wtimeout(self, insert_fn):
        """
        Given a callback function representing an insert operation on
        the primary, handle any connection failures, and keep retrying
        the operation for up to 'AWAIT_REPL_TIMEOUT_MINS' minutes.

        The insert operation callback should take an argument for the
        number of remaining seconds to provide as the timeout for the
        operation.
        """

        deadline = time.time() + ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60

        while True:
            try:
                remaining = deadline - time.time()
                insert_fn(remaining)
                break
            except pymongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    raise errors.ServerFailure("Failed to connect to the primary on port %d" %
                                               self.port)
