"""
Subclasses of unittest.TestCase.
"""

from __future__ import absolute_import

import os
import os.path
import unittest

from .. import config
from .. import core
from .. import logging
from .. import utils


def make_test_case(test_kind, *args, **kwargs):
    """
    Factory function for creating TestCase instances.
    """

    if test_kind not in _TEST_CASES:
        raise ValueError("Unknown test kind '%s'" % (test_kind))
    return _TEST_CASES[test_kind](*args, **kwargs)


class TestCase(unittest.TestCase):
    """
    A test case to execute.
    """

    def __init__(self, logger, test_kind, test_name):
        """
        Initializes the TestCase with the name of the test.
        """

        unittest.TestCase.__init__(self, methodName="run_test")

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        if not isinstance(test_kind, basestring):
            raise TypeError("test_kind must be a string")

        if not isinstance(test_name, basestring):
            raise TypeError("test_name must be a string")

        self.logger = logger
        self.test_kind = test_kind
        self.test_name = test_name

        self.fixture = None
        self.return_code = None

    def long_name(self):
        """
        Returns the path to the test, relative to the current working directory.
        """
        return os.path.relpath(self.test_name)

    def basename(self):
        """
        Returns the basename of the test.
        """
        return os.path.basename(self.test_name)

    def short_name(self):
        """
        Returns the basename of the test without the file extension.
        """
        return os.path.splitext(self.basename())[0]

    def id(self):
        return self.test_name

    def shortDescription(self):
        return "%s %s" % (self.test_kind, self.test_name)

    def configure(self, fixture):
        """
        Stores 'fixture' as an attribute for later use during execution.
        """
        self.fixture = fixture

    def run_test(self):
        """
        Runs the specified test.
        """
        raise NotImplementedError("run_test must be implemented by TestCase subclasses")

    def as_command(self):
        """
        Returns the command invocation used to run the test.
        """
        return self._make_process().as_command()

    def _execute(self, process):
        """
        Runs the specified process.
        """

        self.logger.info("Starting %s...\n%s", self.shortDescription(), process.as_command())
        process.start()
        self.logger.info("%s started with pid %s.", self.shortDescription(), process.pid)

        self.return_code = process.wait()
        if self.return_code != 0:
            raise self.failureException("%s failed" % (self.shortDescription()))

        self.logger.info("%s finished.", self.shortDescription())

    def _make_process(self):
        """
        Returns a new Process instance that could be used to run the
        test or log the command.
        """
        raise NotImplementedError("_make_process must be implemented by TestCase subclasses")


class CPPUnitTestCase(TestCase):
    """
    A C++ unit test to execute.
    """

    def __init__(self,
                 logger,
                 program_executable,
                 program_options=None):
        """
        Initializes the CPPUnitTestCase with the executable to run.
        """

        TestCase.__init__(self, logger, "Program", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def run_test(self):
        try:
            program = self._make_process()
            self._execute(program)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running C++ unit test %s.", self.basename())
            raise

    def _make_process(self):
        return core.process.Process(self.logger,
                                    [self.program_executable],
                                    **self.program_options)


class CPPIntegrationTestCase(TestCase):
    """
    A C++ integration test to execute.
    """

    def __init__(self,
                 logger,
                 program_executable,
                 program_options=None):
        """
        Initializes the CPPIntegrationTestCase with the executable to run.
        """

        TestCase.__init__(self, logger, "Program", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def configure(self, fixture):
        TestCase.configure(self, fixture)

        self.program_options["connectionString"] = self.fixture.get_internal_connection_string()

    def run_test(self):
        try:
            program = self._make_process()
            self._execute(program)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running C++ integration test %s.",
                                  self.basename())
            raise

    def _make_process(self):
        return core.programs.generic_program(self.logger,
                                             [self.program_executable],
                                             **self.program_options)


class DBTestCase(TestCase):
    """
    A dbtest to execute.
    """

    def __init__(self,
                 logger,
                 dbtest_suite,
                 dbtest_executable=None,
                 dbtest_options=None):
        """
        Initializes the DBTestCase with the dbtest suite to run.
        """

        TestCase.__init__(self, logger, "DBTest", dbtest_suite)

        # Command line options override the YAML configuration.
        self.dbtest_executable = utils.default_if_none(config.DBTEST_EXECUTABLE, dbtest_executable)

        self.dbtest_suite = dbtest_suite
        self.dbtest_options = utils.default_if_none(dbtest_options, {}).copy()

    def configure(self, fixture):
        TestCase.configure(self, fixture)

        # If a dbpath was specified, then use it as a container for all other dbpaths.
        dbpath_prefix = self.dbtest_options.pop("dbpath", DBTestCase._get_dbpath_prefix())
        dbpath = os.path.join(dbpath_prefix, "job%d" % (self.fixture.job_num), "unittest")
        self.dbtest_options["dbpath"] = dbpath

        utils.rmtree(dbpath, ignore_errors=True)

        try:
            os.makedirs(dbpath)
        except os.error:
            # Directory already exists.
            pass

    def run_test(self):
        try:
            dbtest = self._make_process()
            self._execute(dbtest)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running dbtest suite %s.", self.basename())
            raise

    def _make_process(self):
        return core.programs.dbtest_program(self.logger,
                                            executable=self.dbtest_executable,
                                            suites=[self.dbtest_suite],
                                            **self.dbtest_options)

    @staticmethod
    def _get_dbpath_prefix():
        """
        Returns the prefix of the dbpath to use for the dbtest
        executable.

        Order of preference:
          1. The --dbpathPrefix specified at the command line.
          2. Value of the TMPDIR environment variable.
          3. Value of the TEMP environment variable.
          4. Value of the TMP environment variable.
          5. The /tmp directory.
        """

        if config.DBPATH_PREFIX is not None:
            return config.DBPATH_PREFIX

        for env_var in ("TMPDIR", "TEMP", "TMP"):
            if env_var in os.environ:
                return os.environ[env_var]
        return os.path.normpath("/tmp")


class JSTestCase(TestCase):
    """
    A jstest to execute.
    """

    def __init__(self,
                 logger,
                 js_filename,
                 shell_executable=None,
                 shell_options=None,
                 use_connection_string=False,
                 test_kind="JSTest"):
        "Initializes the JSTestCase with the JS file to run."

        TestCase.__init__(self, logger, test_kind, js_filename)

        # Command line options override the YAML configuration.
        self.shell_executable = utils.default_if_none(config.MONGO_EXECUTABLE, shell_executable)

        self.js_filename = js_filename
        self.shell_options = utils.default_if_none(shell_options, {}).copy()
        self.use_connection_string = use_connection_string

    def configure(self, fixture):
        TestCase.configure(self, fixture)

        if self.fixture.port is not None:
            self.shell_options["port"] = self.fixture.port

        global_vars = self.shell_options.get("global_vars", {}).copy()
        data_dir = self._get_data_dir(global_vars)

        # Set MongoRunner.dataPath if overridden at command line or not specified in YAML.
        if config.DBPATH_PREFIX is not None or "MongoRunner.dataPath" not in global_vars:
            # dataPath property is the dataDir property with a trailing slash.
            data_path = os.path.join(data_dir, "")
        else:
            data_path = global_vars["MongoRunner.dataPath"]

        global_vars["MongoRunner.dataDir"] = data_dir
        global_vars["MongoRunner.dataPath"] = data_path

        test_data = global_vars.get("TestData", {}).copy()
        test_data["minPort"] = core.network.PortAllocator.min_test_port(fixture.job_num)
        test_data["maxPort"] = core.network.PortAllocator.max_test_port(fixture.job_num)

        global_vars["TestData"] = test_data
        self.shell_options["global_vars"] = global_vars

        utils.rmtree(data_dir, ignore_errors=True)

        try:
            os.makedirs(data_dir)
        except os.error:
            # Directory already exists.
            pass

    def _get_data_dir(self, global_vars):
        """
        Returns the value that the mongo shell should set for the
        MongoRunner.dataDir property.
        """

        # Command line options override the YAML configuration.
        data_dir_prefix = utils.default_if_none(config.DBPATH_PREFIX,
                                                global_vars.get("MongoRunner.dataDir"))
        data_dir_prefix = utils.default_if_none(data_dir_prefix, config.DEFAULT_DBPATH_PREFIX)
        return os.path.join(data_dir_prefix,
                            "job%d" % (self.fixture.job_num),
                            config.MONGO_RUNNER_SUBDIR)

    def run_test(self):
        try:
            shell = self._make_process()
            self._execute(shell)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running jstest %s.", self.basename())
            raise

    def _make_process(self):
        connection_string = None
        if self.use_connection_string:
            connection_string = self.fixture.get_driver_connection_url()

        return core.programs.mongo_shell_program(
            self.logger,
            executable=self.shell_executable,
            filename=self.js_filename,
            connection_string=connection_string,
            **self.shell_options)


class MongosTestCase(TestCase):
    """
    A TestCase which runs a mongos binary with the given parameters.
    """

    def __init__(self,
                 logger,
                 mongos_options):
        """
        Initializes the mongos test and saves the options.
        """

        self.mongos_executable = utils.default_if_none(config.MONGOS_EXECUTABLE,
                                                       config.DEFAULT_MONGOS_EXECUTABLE)
        # Use the executable as the test name.
        TestCase.__init__(self, logger, "mongos", self.mongos_executable)
        self.options = mongos_options.copy()

    def configure(self, fixture):
        """
        Ensures the --test option is present in the mongos options.
        """

        TestCase.configure(self, fixture)
        # Always specify test option to ensure the mongos will terminate.
        if "test" not in self.options:
            self.options["test"] = ""

    def run_test(self):
        try:
            mongos = self._make_process()
            self._execute(mongos)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running %s.", mongos.as_command())
            raise

    def _make_process(self):
        return core.programs.mongos_program(self.logger,
                                            executable=self.mongos_executable,
                                            **self.options)


_TEST_CASES = {
    "cpp_unit_test": CPPUnitTestCase,
    "cpp_integration_test": CPPIntegrationTestCase,
    "db_test": DBTestCase,
    "js_test": JSTestCase,
    "mongos_test": MongosTestCase,
}
