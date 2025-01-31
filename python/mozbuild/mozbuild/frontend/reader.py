# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# This file contains code for reading metadata from the build system into
# data structures.

r"""Read build frontend files into data structures.

In terms of code architecture, the main interface is BuildReader. BuildReader
starts with a root mozbuild file. It creates a new execution environment for
this file, which is represented by the Sandbox class. The Sandbox class is used
to fill a Context, representing the output of an individual mozbuild file. The

The BuildReader contains basic logic for traversing a tree of mozbuild files.
It does this by examining specific variables populated during execution.
"""

from __future__ import print_function, unicode_literals

import inspect
import logging
import os
import sys
import textwrap
import time
import tokenize
import traceback
import types

from collections import OrderedDict
from io import StringIO

from mozbuild.util import (
    memoize,
    ReadOnlyDefaultDict,
    ReadOnlyDict,
)

from mozbuild.backend.configenvironment import ConfigEnvironment

from mozpack.files import FileFinder
import mozpack.path as mozpath

from .data import (
    AndroidEclipseProjectData,
    JavaJarData,
)

from .sandbox import (
    SandboxError,
    SandboxExecutionError,
    SandboxLoadError,
    Sandbox,
)

from .context import (
    Context,
    ContextDerivedValue,
    FUNCTIONS,
    VARIABLES,
    DEPRECATION_HINTS,
    SPECIAL_VARIABLES,
    TemplateContext,
)

if sys.version_info.major == 2:
    text_type = unicode
    type_type = types.TypeType
else:
    text_type = str
    type_type = type


def log(logger, level, action, params, formatter):
    logger.log(level, formatter, extra={'action': action, 'params': params})


def is_read_allowed(path, config):
    """Whether we are allowed to load a mozbuild file at the specified path.

    This is used as cheap security to ensure the build is isolated to known
    source directories.

    We are allowed to read from the main source directory and any defined
    external source directories. The latter is to allow 3rd party applications
    to hook into our build system.
    """
    assert os.path.isabs(path)
    assert os.path.isabs(config.topsrcdir)

    path = mozpath.normpath(path)
    topsrcdir = mozpath.normpath(config.topsrcdir)

    if mozpath.basedir(path, [topsrcdir]):
        return True

    if config.external_source_dir and \
            mozpath.basedir(path, [config.external_source_dir]):
        return True

    return False


class SandboxCalledError(SandboxError):
    """Represents an error resulting from calling the error() function."""

    def __init__(self, file_stack, message):
        SandboxError.__init__(self, file_stack)
        self.message = message


class MozbuildSandbox(Sandbox):
    """Implementation of a Sandbox tailored for mozbuild files.

    We expose a few useful functions and expose the set of variables defining
    Mozilla's build system.

    context is a Context instance.

    metadata is a dict of metadata that can be used during the sandbox
    evaluation.
    """
    def __init__(self, context, metadata={}):
        assert isinstance(context, Context)

        Sandbox.__init__(self, context)

        self._log = logging.getLogger(__name__)

        self.metadata = dict(metadata)
        exports = self.metadata.get('exports', {})
        self.exports = set(exports.keys())
        context.update(exports)
        self.templates = self.metadata.setdefault('templates', {})

    def __getitem__(self, key):
        if key in SPECIAL_VARIABLES:
            return SPECIAL_VARIABLES[key][0](self._context)
        if key in FUNCTIONS:
            return self._create_function(FUNCTIONS[key])
        if key in self.templates:
            return self._create_template_function(self.templates[key])
        return Sandbox.__getitem__(self, key)

    def __setitem__(self, key, value):
        if key in SPECIAL_VARIABLES or key in FUNCTIONS:
            raise KeyError()
        if key in self.exports:
            self._context[key] = value
            self.exports.remove(key)
            return
        Sandbox.__setitem__(self, key, value)

    def exec_file(self, path):
        """Override exec_file to normalize paths and restrict file loading.

        Paths will be rejected if they do not fall under topsrcdir or one of
        the external roots.
        """

        # realpath() is needed for true security. But, this isn't for security
        # protection, so it is omitted.
        if not is_read_allowed(path, self._context.config):
            raise SandboxLoadError(self._context.source_stack,
                sys.exc_info()[2], illegal_path=path)

        Sandbox.exec_file(self, path)

    def _add_java_jar(self, name):
        """Add a Java JAR build target."""
        if not name:
            raise Exception('Java JAR cannot be registered without a name')

        if '/' in name or '\\' in name or '.jar' in name:
            raise Exception('Java JAR names must not include slashes or'
                ' .jar: %s' % name)

        if name in self['JAVA_JAR_TARGETS']:
            raise Exception('Java JAR has already been registered: %s' % name)

        jar = JavaJarData(name)
        self['JAVA_JAR_TARGETS'][name] = jar
        return jar

    # Not exposed to the sandbox.
    def add_android_eclipse_project_helper(self, name):
        """Add an Android Eclipse project target."""
        if not name:
            raise Exception('Android Eclipse project cannot be registered without a name')

        if name in self['ANDROID_ECLIPSE_PROJECT_TARGETS']:
            raise Exception('Android Eclipse project has already been registered: %s' % name)

        data = AndroidEclipseProjectData(name)
        self['ANDROID_ECLIPSE_PROJECT_TARGETS'][name] = data
        return data

    def _add_android_eclipse_project(self, name, manifest):
        if not manifest:
            raise Exception('Android Eclipse project must specify a manifest')

        data = self.add_android_eclipse_project_helper(name)
        data.manifest = manifest
        data.is_library = False
        return data

    def _add_android_eclipse_library_project(self, name):
        data = self.add_android_eclipse_project_helper(name)
        data.manifest = None
        data.is_library = True
        return data

    def _export(self, varname):
        """Export the variable to all subdirectories of the current path."""

        exports = self.metadata.setdefault('exports', dict())
        if varname in exports:
            raise Exception('Variable has already been exported: %s' % varname)

        try:
            # Doing a regular self._context[varname] causes a set as a side
            # effect. By calling the dict method instead, we don't have any
            # side effects.
            exports[varname] = dict.__getitem__(self._context, varname)
        except KeyError:
            self.last_name_error = KeyError('global_ns', 'get_unknown', varname)
            raise self.last_name_error

    def recompute_exports(self):
        """Recompute the variables to export to subdirectories with the current
        values in the subdirectory."""

        if 'exports' in self.metadata:
            for key in self.metadata['exports']:
                self.metadata['exports'][key] = self[key]

    def _include(self, path):
        """Include and exec another file within the context of this one."""

        # path is a SourcePath, and needs to be coerced to unicode.
        self.exec_file(unicode(path))

    def _warning(self, message):
        # FUTURE consider capturing warnings in a variable instead of printing.
        print('WARNING: %s' % message, file=sys.stderr)

    def _error(self, message):
        raise SandboxCalledError(self._context.source_stack, message)

    def _template_decorator(self, func):
        """Registers template as expected by _create_template_function.

        The template data consists of:
        - the function object as it comes from the sandbox evaluation of the
          template declaration.
        - its code, modified as described in the comments of this method.
        - the path of the file containing the template definition.
        """

        if not inspect.isfunction(func):
            raise Exception('`template` is a function decorator. You must '
                'use it as `@template` preceding a function declaration.')

        name = func.func_name

        if name in self.templates:
            raise KeyError(
                'A template named "%s" was already declared in %s.' % (name,
                self.templates[name][2]))

        if name.islower() or name.isupper() or name[0].islower():
            raise NameError('Template function names must be CamelCase.')

        lines, firstlineno = inspect.getsourcelines(func)
        first_op = None
        generator = tokenize.generate_tokens(iter(lines).next)
        # Find the first indent token in the source of this template function,
        # which corresponds to the beginning of the function body.
        for typ, s, begin, end, line in generator:
            if typ == tokenize.OP:
                first_op = True
            if first_op and typ == tokenize.INDENT:
                break
        if typ != tokenize.INDENT:
            # This should never happen.
            raise Exception('Could not find the first line of the template %s' %
                func.func_name)
        # The code of the template in moz.build looks like this:
        # m      def Foo(args):
        # n          FOO = 'bar'
        # n+1        (...)
        #
        # where,
        # - m is firstlineno - 1,
        # - n is usually m + 1, but in case the function signature takes more
        # lines, is really m + begin[0] - 1
        #
        # We want that to be replaced with:
        # m       if True:
        # n           FOO = 'bar'
        # n+1         (...)
        #
        # (this is simpler than trying to deindent the function body)
        # So we need to prepend with n - 1 newlines so that line numbers
        # are unchanged.
        code = '\n' * (firstlineno + begin[0] - 3) + 'if True:\n'
        code += ''.join(lines[begin[0] - 1:])

        self.templates[name] = func, code, self._context.current_path

    @memoize
    def _create_function(self, function_def):
        """Returns a function object for use within the sandbox for the given
        function definition.

        The wrapper function does type coercion on the function arguments
        """
        func, args_def, doc = function_def
        def function(*args):
            def coerce(arg, type):
                if not isinstance(arg, type):
                    if issubclass(type, ContextDerivedValue):
                        arg = type(self._context, arg)
                    else:
                        arg = type(arg)
                return arg
            args = [coerce(arg, type) for arg, type in zip(args, args_def)]
            return func(self)(*args)

        return function

    @memoize
    def _create_template_function(self, template):
        """Returns a function object for use within the sandbox for the given
        template.

        When a moz.build file contains a reference to a template call, the
        sandbox needs a function to execute. This is what this method returns.
        That function creates a new sandbox for execution of the template.
        After the template is executed, the data from its execution is merged
        with the context of the calling sandbox.
        """
        func, code, path = template

        def template_function(*args, **kwargs):
            context = TemplateContext(VARIABLES, self._context.config)
            context.add_source(self._context.current_path)
            for p in self._context.all_paths:
                context.add_source(p)

            sandbox = MozbuildSandbox(context, self.metadata)
            for k, v in inspect.getcallargs(func, *args, **kwargs).items():
                sandbox[k] = v

            sandbox.exec_source(code, path)

            # This is gross, but allows the merge to happen. Eventually, the
            # merging will go away and template contexts emitted independently.
            klass = self._context.__class__
            self._context.__class__ = TemplateContext
            # The sandbox will do all the necessary checks for these merges.
            for key, value in context.items():
                if isinstance(value, dict):
                    self[key].update(value)
                elif isinstance(value, list):
                    self[key] += value
                else:
                    self[key] = value
            self._context.__class__ = klass

            for p in context.all_paths:
                self._context.add_source(p)

        return template_function


class SandboxValidationError(Exception):
    """Represents an error encountered when validating sandbox results."""
    def __init__(self, message, context):
        Exception.__init__(self, message)
        self.context = context

    def __str__(self):
        s = StringIO()

        delim = '=' * 30
        s.write('\n%s\nERROR PROCESSING MOZBUILD FILE\n%s\n\n' % (delim, delim))

        s.write('The error occurred while processing the following file or ')
        s.write('one of the files it includes:\n')
        s.write('\n')
        s.write('    %s/moz.build\n' % self.context.srcdir)
        s.write('\n')

        s.write('The error occurred when validating the result of ')
        s.write('the execution. The reported error is:\n')
        s.write('\n')
        s.write(''.join('    %s\n' % l
                        for l in self.message.splitlines()))
        s.write('\n')

        return s.getvalue()


class BuildReaderError(Exception):
    """Represents errors encountered during BuildReader execution.

    The main purpose of this class is to facilitate user-actionable error
    messages. Execution errors should say:

      - Why they failed
      - Where they failed
      - What can be done to prevent the error

    A lot of the code in this class should arguably be inside sandbox.py.
    However, extraction is somewhat difficult given the additions
    MozbuildSandbox has over Sandbox (e.g. the concept of included files -
    which affect error messages, of course).
    """
    def __init__(self, file_stack, trace, sandbox_exec_error=None,
        sandbox_load_error=None, validation_error=None, other_error=None,
        sandbox_called_error=None):

        self.file_stack = file_stack
        self.trace = trace
        self.sandbox_called_error = sandbox_called_error
        self.sandbox_exec = sandbox_exec_error
        self.sandbox_load = sandbox_load_error
        self.validation_error = validation_error
        self.other = other_error

    @property
    def main_file(self):
        return self.file_stack[-1]

    @property
    def actual_file(self):
        # We report the file that called out to the file that couldn't load.
        if self.sandbox_load is not None:
            if len(self.sandbox_load.file_stack) > 1:
                return self.sandbox_load.file_stack[-2]

            if len(self.file_stack) > 1:
                return self.file_stack[-2]

        if self.sandbox_error is not None and \
            len(self.sandbox_error.file_stack):
            return self.sandbox_error.file_stack[-1]

        return self.file_stack[-1]

    @property
    def sandbox_error(self):
        return self.sandbox_exec or self.sandbox_load or \
            self.sandbox_called_error

    def __str__(self):
        s = StringIO()

        delim = '=' * 30
        s.write('\n%s\nERROR PROCESSING MOZBUILD FILE\n%s\n\n' % (delim, delim))

        s.write('The error occurred while processing the following file:\n')
        s.write('\n')
        s.write('    %s\n' % self.actual_file)
        s.write('\n')

        if self.actual_file != self.main_file and not self.sandbox_load:
            s.write('This file was included as part of processing:\n')
            s.write('\n')
            s.write('    %s\n' % self.main_file)
            s.write('\n')

        if self.sandbox_error is not None:
            self._print_sandbox_error(s)
        elif self.validation_error is not None:
            s.write('The error occurred when validating the result of ')
            s.write('the execution. The reported error is:\n')
            s.write('\n')
            s.write(''.join('    %s\n' % l
                            for l in self.validation_error.message.splitlines()))
            s.write('\n')
        else:
            s.write('The error appears to be part of the %s ' % __name__)
            s.write('Python module itself! It is possible you have stumbled ')
            s.write('across a legitimate bug.\n')
            s.write('\n')

            for l in traceback.format_exception(type(self.other), self.other,
                self.trace):
                s.write(unicode(l))

        return s.getvalue()

    def _print_sandbox_error(self, s):
        # Try to find the frame of the executed code.
        script_frame = None

        # We don't currently capture the trace for SandboxCalledError.
        # Therefore, we don't get line numbers from the moz.build file.
        # FUTURE capture this.
        trace = getattr(self.sandbox_error, 'trace', None)
        frames = []
        if trace:
            frames = traceback.extract_tb(trace)
        for frame in frames:
            if frame[0] == self.actual_file:
                script_frame = frame

            # Reset if we enter a new execution context. This prevents errors
            # in this module from being attributes to a script.
            elif frame[0] == __file__ and frame[2] == 'exec_source':
                script_frame = None

        if script_frame is not None:
            s.write('The error was triggered on line %d ' % script_frame[1])
            s.write('of this file:\n')
            s.write('\n')
            s.write('    %s\n' % script_frame[3])
            s.write('\n')

        if self.sandbox_called_error is not None:
            self._print_sandbox_called_error(s)
            return

        if self.sandbox_load is not None:
            self._print_sandbox_load_error(s)
            return

        self._print_sandbox_exec_error(s)

    def _print_sandbox_called_error(self, s):
        assert self.sandbox_called_error is not None

        s.write('A moz.build file called the error() function.\n')
        s.write('\n')
        s.write('The error it encountered is:\n')
        s.write('\n')
        s.write('    %s\n' % self.sandbox_called_error.message)
        s.write('\n')
        s.write('Correct the error condition and try again.\n')

    def _print_sandbox_load_error(self, s):
        assert self.sandbox_load is not None

        if self.sandbox_load.illegal_path is not None:
            s.write('The underlying problem is an illegal file access. ')
            s.write('This is likely due to trying to access a file ')
            s.write('outside of the top source directory.\n')
            s.write('\n')
            s.write('The path whose access was denied is:\n')
            s.write('\n')
            s.write('    %s\n' % self.sandbox_load.illegal_path)
            s.write('\n')
            s.write('Modify the script to not access this file and ')
            s.write('try again.\n')
            return

        if self.sandbox_load.read_error is not None:
            if not os.path.exists(self.sandbox_load.read_error):
                s.write('The underlying problem is we referenced a path ')
                s.write('that does not exist. That path is:\n')
                s.write('\n')
                s.write('    %s\n' % self.sandbox_load.read_error)
                s.write('\n')
                s.write('Either create the file if it needs to exist or ')
                s.write('do not reference it.\n')
            else:
                s.write('The underlying problem is a referenced path could ')
                s.write('not be read. The trouble path is:\n')
                s.write('\n')
                s.write('    %s\n' % self.sandbox_load.read_error)
                s.write('\n')
                s.write('It is possible the path is not correct. Is it ')
                s.write('pointing to a directory? It could also be a file ')
                s.write('permissions issue. Ensure that the file is ')
                s.write('readable.\n')

            return

        # This module is buggy if you see this.
        raise AssertionError('SandboxLoadError with unhandled properties!')

    def _print_sandbox_exec_error(self, s):
        assert self.sandbox_exec is not None

        inner = self.sandbox_exec.exc_value

        if isinstance(inner, SyntaxError):
            s.write('The underlying problem is a Python syntax error ')
            s.write('on line %d:\n' % inner.lineno)
            s.write('\n')
            s.write('    %s\n' % inner.text)
            if inner.offset:
                s.write((' ' * (inner.offset + 4)) + '^\n')
            s.write('\n')
            s.write('Fix the syntax error and try again.\n')
            return

        if isinstance(inner, KeyError):
            self._print_keyerror(inner, s)
        elif isinstance(inner, ValueError):
            self._print_valueerror(inner, s)
        else:
            self._print_exception(inner, s)

    def _print_keyerror(self, inner, s):
        if inner.args[0] not in ('global_ns', 'local_ns'):
            self._print_exception(inner, s)
            return

        if inner.args[0] == 'global_ns':
            import difflib

            verb = None
            if inner.args[1] == 'get_unknown':
                verb = 'read'
            elif inner.args[1] == 'set_unknown':
                verb = 'write'
            elif inner.args[1] == 'reassign':
                s.write('The underlying problem is an attempt to reassign ')
                s.write('a reserved UPPERCASE variable.\n')
                s.write('\n')
                s.write('The reassigned variable causing the error is:\n')
                s.write('\n')
                s.write('    %s\n' % inner.args[2])
                s.write('\n')
                s.write('Maybe you meant "+=" instead of "="?\n')
                return
            else:
                raise AssertionError('Unhandled global_ns: %s' % inner.args[1])

            s.write('The underlying problem is an attempt to %s ' % verb)
            s.write('a reserved UPPERCASE variable that does not exist.\n')
            s.write('\n')
            s.write('The variable %s causing the error is:\n' % verb)
            s.write('\n')
            s.write('    %s\n' % inner.args[2])
            s.write('\n')
            close_matches = difflib.get_close_matches(inner.args[2],
                                                      VARIABLES.keys(), 2)
            if close_matches:
                s.write('Maybe you meant %s?\n' % ' or '.join(close_matches))
                s.write('\n')

            if inner.args[2] in DEPRECATION_HINTS:
                s.write('%s\n' %
                    textwrap.dedent(DEPRECATION_HINTS[inner.args[2]]).strip())
                return

            s.write('Please change the file to not use this variable.\n')
            s.write('\n')
            s.write('For reference, the set of valid variables is:\n')
            s.write('\n')
            s.write(', '.join(sorted(VARIABLES.keys())) + '\n')
            return

        s.write('The underlying problem is a reference to an undefined ')
        s.write('local variable:\n')
        s.write('\n')
        s.write('    %s\n' % inner.args[2])
        s.write('\n')
        s.write('Please change the file to not reference undefined ')
        s.write('variables and try again.\n')

    def _print_valueerror(self, inner, s):
        if inner.args[0] not in ('global_ns', 'local_ns'):
            self._print_exception(inner, s)
            return

        assert inner.args[1] == 'set_type'

        s.write('The underlying problem is an attempt to write an illegal ')
        s.write('value to a special variable.\n')
        s.write('\n')
        s.write('The variable whose value was rejected is:\n')
        s.write('\n')
        s.write('    %s' % inner.args[2])
        s.write('\n')
        s.write('The value being written to it was of the following type:\n')
        s.write('\n')
        s.write('    %s\n' % type(inner.args[3]).__name__)
        s.write('\n')
        s.write('This variable expects the following type(s):\n')
        s.write('\n')
        if type(inner.args[4]) == type_type:
            s.write('    %s\n' % inner.args[4].__name__)
        else:
            for t in inner.args[4]:
                s.write( '    %s\n' % t.__name__)
        s.write('\n')
        s.write('Change the file to write a value of the appropriate type ')
        s.write('and try again.\n')

    def _print_exception(self, e, s):
        s.write('An error was encountered as part of executing the file ')
        s.write('itself. The error appears to be the fault of the script.\n')
        s.write('\n')
        s.write('The error as reported by Python is:\n')
        s.write('\n')
        s.write('    %s\n' % traceback.format_exception_only(type(e), e))


class BuildReader(object):
    """Read a tree of mozbuild files into data structures.

    This is where the build system starts. You give it a tree configuration
    (the output of configuration) and it executes the moz.build files and
    collects the data they define.

    The reader can optionally call a callable after each sandbox is evaluated
    but before its evaluated content is processed. This gives callers the
    opportunity to modify contexts before side-effects occur from their
    content. This callback receives the ``Context`` containing the result of
    each sandbox evaluation. Its return value is ignored.
    """

    def __init__(self, config, sandbox_post_eval_cb=None):
        self.config = config

        self._sandbox_post_eval_cb = sandbox_post_eval_cb
        self._log = logging.getLogger(__name__)
        self._read_files = set()
        self._execution_stack = []

    def read_topsrcdir(self):
        """Read the tree of linked moz.build files.

        This starts with the tree's top-most moz.build file and descends into
        all linked moz.build files until all relevant files have been evaluated.

        This is a generator of Context instances. As each moz.build file is
        read, a new Context is created and emitted.
        """
        path = mozpath.join(self.config.topsrcdir, 'moz.build')
        return self.read_mozbuild(path, self.config, read_tiers=True)

    def walk_topsrcdir(self):
        """Read all moz.build files in the source tree.

        This is different from read_topsrcdir() in that this version performs a
        filesystem walk to discover every moz.build file rather than relying on
        data from executed moz.build files to drive traversal.

        This is a generator of Context instances.
        """
        # In the future, we may traverse moz.build files by looking
        # for DIRS references in the AST, even if a directory is added behind
        # a conditional. For now, just walk the filesystem.
        ignore = {
            # Ignore fake moz.build files used for testing moz.build.
            'python/mozbuild/mozbuild/test',

            # Ignore object directories.
            'obj*',
        }

        finder = FileFinder(self.config.topsrcdir, find_executables=False,
            ignore=ignore)

        for path, f in finder.find('**/moz.build'):
            path = os.path.join(self.config.topsrcdir, path)
            for s in self.read_mozbuild(path, self.config, descend=False,
                read_tiers=True):
                yield s

    def read_mozbuild(self, path, config, read_tiers=False, descend=True,
            metadata={}):
        """Read and process a mozbuild file, descending into children.

        This starts with a single mozbuild file, executes it, and descends into
        other referenced files per our traversal logic.

        The traversal logic is to iterate over the *DIRS variables, treating
        each element as a relative directory path. For each encountered
        directory, we will open the moz.build file located in that
        directory in a new Sandbox and process it.

        If read_tiers is True (it should only be True for the top-level
        mozbuild file in a project), the TIERS variable will be used for
        traversal as well.

        If descend is True (the default), we will descend into child
        directories and files per variable values.

        Arbitrary metadata in the form of a dict can be passed into this
        function. This feature is intended to facilitate the build reader
        injecting state and annotations into moz.build files that is
        independent of the sandbox's execution context.

        Traversal is performed depth first (for no particular reason).
        """
        self._execution_stack.append(path)
        try:
            for s in self._read_mozbuild(path, config, read_tiers=read_tiers,
                descend=descend, metadata=metadata):
                yield s

        except BuildReaderError as bre:
            raise bre

        except SandboxCalledError as sce:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], sandbox_called_error=sce)

        except SandboxExecutionError as se:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], sandbox_exec_error=se)

        except SandboxLoadError as sle:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], sandbox_load_error=sle)

        except SandboxValidationError as ve:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], validation_error=ve)

        except Exception as e:
            raise BuildReaderError(list(self._execution_stack),
                sys.exc_info()[2], other_error=e)

    def _read_mozbuild(self, path, config, read_tiers, descend, metadata):
        path = mozpath.normpath(path)
        log(self._log, logging.DEBUG, 'read_mozbuild', {'path': path},
            'Reading file: {path}')

        if path in self._read_files:
            log(self._log, logging.WARNING, 'read_already', {'path': path},
                'File already read. Skipping: {path}')
            return

        self._read_files.add(path)

        time_start = time.time()

        topobjdir = config.topobjdir

        if not mozpath.basedir(path, [config.topsrcdir]):
            external = config.external_source_dir
            if external and mozpath.basedir(path, [external]):
                config = ConfigEnvironment.from_config_status(
                    mozpath.join(topobjdir, 'config.status'))
                config.topsrcdir = external
                config.external_source_dir = None

        relpath = mozpath.relpath(path, config.topsrcdir)
        reldir = mozpath.dirname(relpath)

        if mozpath.dirname(relpath) == 'js/src' and \
                not config.substs.get('JS_STANDALONE'):
            config = ConfigEnvironment.from_config_status(
                mozpath.join(topobjdir, reldir, 'config.status'))
            config.topobjdir = topobjdir
            config.external_source_dir = None

        context = Context(VARIABLES, config)
        sandbox = MozbuildSandbox(context, metadata=metadata)
        sandbox.exec_file(path)
        context.execution_time = time.time() - time_start

        if self._sandbox_post_eval_cb:
            self._sandbox_post_eval_cb(context)

        # We first collect directories populated in variables.
        dir_vars = ['DIRS']

        if context.config.substs.get('ENABLE_TESTS', False) == '1':
            dir_vars.append('TEST_DIRS')

        dirs = [(v, context[v]) for v in dir_vars if v in context]

        curdir = mozpath.dirname(path)

        gyp_contexts = []
        for target_dir in context['GYP_DIRS']:
            gyp_dir = context['GYP_DIRS'][target_dir]
            for v in ('input', 'variables'):
                if not getattr(gyp_dir, v):
                    raise SandboxValidationError('Missing value for '
                        'GYP_DIRS["%s"].%s' % (target_dir, v), context)

            # The make backend assumes contexts for sub-directories are
            # emitted after their parent, so accumulate the gyp contexts.
            # We could emit the parent context before processing gyp
            # configuration, but we need to add the gyp objdirs to that context
            # first.
            from .gyp_reader import read_from_gyp
            non_unified_sources = set()
            for s in gyp_dir.non_unified_sources:
                source = mozpath.normpath(mozpath.join(curdir, s))
                if not os.path.exists(source):
                    raise SandboxValidationError('Cannot find %s.' % source,
                        context)
                non_unified_sources.add(source)
            for gyp_context in read_from_gyp(context.config,
                                             mozpath.join(curdir, gyp_dir.input),
                                             mozpath.join(context.objdir,
                                                          target_dir),
                                             gyp_dir.variables,
                                             non_unified_sources = non_unified_sources):
                gyp_context.update(gyp_dir.sandbox_vars)
                gyp_contexts.append(gyp_context)

        for gyp_context in gyp_contexts:
            if self._sandbox_post_eval_cb:
                self._sandbox_post_eval_cb(gyp_context)

            context['DIRS'].append(mozpath.relpath(gyp_context.objdir, context.objdir))

        yield context

        for gyp_context in gyp_contexts:
            yield gyp_context

        # Traverse into referenced files.

        # It's very tempting to use a set here. Unfortunately, the recursive
        # make backend needs order preserved. Once we autogenerate all backend
        # files, we should be able to convert this to a set.
        recurse_info = OrderedDict()
        for var, var_dirs in dirs:
            for d in var_dirs:
                if d in recurse_info:
                    raise SandboxValidationError(
                        'Directory (%s) registered multiple times in %s' % (
                            mozpath.relpath(d, context.srcdir), var), context)

                recurse_info[d] = {}
                if 'templates' in sandbox.metadata:
                    recurse_info[d]['templates'] = dict(
                        sandbox.metadata['templates'])
                if 'exports' in sandbox.metadata:
                    sandbox.recompute_exports()
                    recurse_info[d]['exports'] = dict(sandbox.metadata['exports'])

        for path, child_metadata in recurse_info.items():
            child_path = path.join('moz.build')

            # Ensure we don't break out of the topsrcdir. We don't do realpath
            # because it isn't necessary. If there are symlinks in the srcdir,
            # that's not our problem. We're not a hosted application: we don't
            # need to worry about security too much.
            if not is_read_allowed(child_path, context.config):
                raise SandboxValidationError(
                    'Attempting to process file outside of allowed paths: %s' %
                        child_path, context)

            if not descend:
                continue

            for res in self.read_mozbuild(child_path, context.config,
                read_tiers=False, metadata=child_metadata):
                yield res

        self._execution_stack.pop()
